#include "media/LivePipeline.h"
#include "media/M2tsPacketizer.h"
#include "media/SourceGapLog.h"
#include "publish/MoqxrPublisher.h"
#include "ts_test_builder.h"

#include <QJsonArray>
#include <QStringList>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <functional>
#include <iostream>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using moq2ts::IMoqOutput;
using moq2ts::PublishConfig;
using moq2ts::PublishedObject;
namespace tb = moq2ts::test;

namespace {
bool expect(bool cond, const std::string& msg) {
    if (!cond) { std::cerr << "FAIL: " << msg << '\n'; return false; }
    return true;
}

const auto kWait = std::chrono::seconds(20);

// A packet on pid that carries a PCR (27 MHz) in its adaptation field.
QByteArray pcrPacket(int pid, std::int64_t pcr, int cc) {
    const std::int64_t base = pcr / 300;
    const int extension = static_cast<int>(pcr % 300);
    QByteArray packet;
    packet.append(char(0x47));
    packet.append(static_cast<char>((pid >> 8) & 0x1F));
    packet.append(static_cast<char>(pid & 0xFF));
    packet.append(static_cast<char>(0x30 | (cc & 0x0F)));
    packet.append(char(7));                                    // adaptation_field_length
    packet.append(char(0x10));                                 // PCR_flag
    packet.append(static_cast<char>((base >> 25) & 0xFF));
    packet.append(static_cast<char>((base >> 17) & 0xFF));
    packet.append(static_cast<char>((base >> 9) & 0xFF));
    packet.append(static_cast<char>((base >> 1) & 0xFF));
    packet.append(static_cast<char>(((base & 0x01) << 7) | 0x7E | ((extension >> 8) & 0x01)));
    packet.append(static_cast<char>(extension & 0xFF));
    packet.append(QByteArray(188 - packet.size(), char(0)));
    return packet;
}

// A single-program stream: program 1, PMT PID 0x1000, PCR and video PID
// 0x100. Each round of 0.1 s of PCR time repeats the PAT and the PMT and
// gives the program a PCR, a random access PES packet with a PTS, and eight
// more video packets, which start with the round number (see roundOf).
QByteArray singleProgramStream(int rounds) {
    int patCc = 0;
    int pmtCc = 0;
    int videoCc = 0;
    QByteArray ts;
    for (int round = 0; round < rounds; ++round) {
        ts += tb::psiPacket(0, tb::patSection({{1, 0x1000}}), patCc++);
        ts += tb::psiPacket(0x1000, tb::pmtSection(1, 0x100, {{0x1B, 0x100}}), pmtCc++);
        ts += pcrPacket(0x100, static_cast<std::int64_t>(round) * 2700000, videoCc++);
        ts += tb::tsPacket(0x100, true, tb::pesHeaderWithPts(static_cast<std::uint64_t>(round) * 9000), videoCc++, 0x40);
        for (int k = 0; k < 8; ++k) {
            QByteArray payload(184, char(k));
            payload[0] = static_cast<char>((round >> 8) & 0xFF);
            payload[1] = static_cast<char>(round & 0xFF);
            ts += tb::tsPacket(0x100, false, payload, videoCc++);
        }
    }
    return ts;
}

// singleProgramStream() padded to a constant rate, as an encoder's SRT
// contribution feed is: 20 null packets per round after the 12 others.
QByteArray constantRateStream(int rounds) {
    const QByteArray program = singleProgramStream(rounds);
    const QByteArray null = tb::tsPacket(0x1FFF, false, QByteArray(184, char(0xFF)));
    QByteArray ts;
    for (int round = 0; round < rounds; ++round) {
        ts += program.mid(static_cast<qsizetype>(round) * 12 * 188, 12 * 188);
        for (int k = 0; k < 20; ++k) {
            ts += null;
        }
    }
    return ts;
}

// constantRateStream() whose PAT lists no program, and whose PMT is absent,
// from round emptyFrom to round emptyTo, as an encoder sends while it restarts.
// With pcrStops, the PCR is absent too.
QByteArray restartingStream(int rounds, int emptyFrom, int emptyTo, bool pcrStops = false) {
    const QByteArray ts = constantRateStream(rounds);
    const QByteArray emptyPat = tb::psiPacket(0, tb::patSection({}));
    const QByteArray null = tb::tsPacket(0x1FFF, false, QByteArray(184, char(0xFF)));
    QByteArray out;
    const qsizetype roundBytes = 32 * 188;
    for (int round = 0; round < rounds; ++round) {
        QByteArray chunk = ts.mid(static_cast<qsizetype>(round) * roundBytes, roundBytes);
        if (round >= emptyFrom && round < emptyTo) {
            chunk.replace(0, 188, emptyPat);    // the PAT
            chunk.replace(188, 188, null);      // the PMT
            if (pcrStops) {
                chunk.replace(2 * 188, 188, null);   // the PCR
            }
        }
        out += chunk;
    }
    return out;
}

// The size of the first rounds of singleProgramStream().
qsizetype roundsBytes(int rounds) {
    return static_cast<qsizetype>(rounds) * 12 * 188;
}

// Opens the FIFO at path for writing once a reader opens it, giving up after
// kWait without one. Returns a blocking descriptor, or -1.
int openWriter(const QString& path) {
    const auto deadline = std::chrono::steady_clock::now() + kWait;
    int fd = -1;
    while (fd < 0 && std::chrono::steady_clock::now() < deadline) {
        fd = ::open(path.toLocal8Bit().constData(), O_WRONLY | O_NONBLOCK);
        if (fd < 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
    if (fd >= 0) {
        ::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL) & ~O_NONBLOCK);
    }
    return fd;
}

// Writes data, stopping early when the reader closed.
void writeAll(int fd, const QByteArray& data) {
    qsizetype done = 0;
    while (done < data.size()) {
        const ssize_t n = ::write(fd, data.constData() + done, data.size() - done);
        if (n <= 0) {
            break;
        }
        done += n;
    }
}

// Keeps the catalog and every Object that LivePipeline hands it, and can run
// a callback once the first Object is sent, on the publishing thread.
class TestPublisher final : public IMoqOutput {
public:
    bool connect(const PublishConfig&) override {
        m_connected = true;
        return true;
    }
    bool connected() const override { return m_connected; }
    void stop() override { m_connected = false; }

    bool publishLiveObjects(const PublishConfig&, const QString&, const QStringList&, const QByteArray& catalog,
                            std::function<std::optional<PublishedObject>()> nextObject,
                            std::atomic<bool>& running) override {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_catalog = catalog;
            ++m_sessions;
        }
        while (running.load(std::memory_order_acquire)) {
            std::optional<PublishedObject> object = nextObject();
            if (!object.has_value()) {
                return true;
            }
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_sent.push_back(std::move(*object));
            }
            if (afterFirstObject) {
                std::exchange(afterFirstObject, {})();
            }
        }
        return true;
    }

    // Runs once the first Object is sent, on the publishing thread.
    std::function<void()> afterFirstObject;

    // The Objects sent on one track, in order.
    std::vector<PublishedObject> sent(const QString& track) const {
        std::lock_guard<std::mutex> lock(m_mutex);
        std::vector<PublishedObject> objects;
        for (const PublishedObject& object : m_sent) {
            if (object.trackName == track) {
                objects.push_back(object);
            }
        }
        return objects;
    }
    // The publish sessions LivePipeline started.
    int sessions() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_sessions;
    }
    // The catalog the session started with.
    QByteArray catalog() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_catalog;
    }

private:
    bool m_connected = false;
    mutable std::mutex m_mutex;
    std::vector<PublishedObject> m_sent;
    QByteArray m_catalog;
    int m_sessions = 0;
};

QJsonObject catalogTrack(const QByteArray& catalog, const QString& name) {
    for (const QJsonValue& track : QJsonDocument::fromJson(catalog).object().value(QStringLiteral("tracks")).toArray()) {
        if (track.toObject().value(QStringLiteral("name")).toString() == name) {
            return track.toObject();
        }
    }
    return {};
}

// The 188-octet packets of an Object payload.
QList<QByteArray> packetsOf(const QByteArray& payload) {
    QList<QByteArray> packets;
    for (qsizetype offset = 0; offset + 188 <= payload.size(); offset += 188) {
        packets.append(payload.mid(offset, 188));
    }
    return packets;
}

int pidOf(const QByteArray& packet) {
    return ((static_cast<unsigned char>(packet[1]) & 0x1F) << 8) | static_cast<unsigned char>(packet[2]);
}

// The flags octet of the adaptation field, or 0 without one.
int adaptationFlags(const QByteArray& packet) {
    const bool adaptation = ((static_cast<unsigned char>(packet[3]) >> 4) & 0x03) >= 2;
    return adaptation && static_cast<unsigned char>(packet[4]) >= 1 ? static_cast<unsigned char>(packet[5]) : 0;
}

// The singleProgramStream round of a packet on the video PID: from its PCR,
// its PTS, or the round number that starts its payload.
int roundOf(const QByteArray& packet) {
    const std::int64_t pcr = moq2ts::pcrOf(packet);
    if (pcr >= 0) {
        return static_cast<int>(pcr / 2700000);
    }
    const std::int64_t pts = moq2ts::pesPts(packet);
    if (pts >= 0) {
        return static_cast<int>(pts / 9000);
    }
    return (static_cast<unsigned char>(packet[4]) << 8) | static_cast<unsigned char>(packet[5]);
}
}  // namespace

// The warnings of the run, kept to check what they name.
std::mutex g_warningMutex;
QStringList g_warnings;

void captureWarning(QtMsgType type, const QMessageLogContext&, const QString& message) {
    if (type == QtWarningMsg) {
        std::lock_guard<std::mutex> lock(g_warningMutex);
        g_warnings.append(message);
    }
}

int main() {
    std::signal(SIGPIPE, SIG_IGN);
    qInstallMessageHandler(captureWarning);
    bool ok = true;
    QTemporaryDir dir;

    // LivePipeline, single-program SRT ingest in per-program carriage: a FIFO
    // with a SourceGapLog, as SRT ingest gives it. The source skips ahead
    // mid-GOP (round 30 to round 40), either before the first Object or after
    // it. Every Group starts at a random access point and the catalog
    // declares mpeg2tsRandomAccess (MSFTS 5.2). Only the first PCR after a gap
    // that follows a published Group has discontinuity_indicator (MSFTS 5.4).
    for (const bool beforeFirst : {true, false}) {
        const std::string label = std::string("single-program SRT, gap ") +
                                  (beforeFirst ? "before the first Object: " : "after the first Object: ");
        const QByteArray full = singleProgramStream(70);
        const qsizetype cut = roundsBytes(30) + (2 + 2 + 4) * 188;
        const qsizetype resume = roundsBytes(40) + (2 + 2) * 188;
        const QByteArray data = full.left(cut) + full.mid(resume);
        const QString fifo = dir.filePath(beforeFirst ? "single-gap-first.fifo" : "single-gap-mid.fifo");
        ::mkfifo(fifo.toLocal8Bit().constData(), 0600);
        std::thread writer([&] {
            const int fd = openWriter(fifo);
            if (fd >= 0) {
                writeAll(fd, data);
                ::close(fd);
            }
        });
        {
            std::lock_guard<std::mutex> lock(g_warningMutex);
            g_warnings.clear();
        }
        auto gaps = std::make_shared<moq2ts::SourceGapLog>();
        TestPublisher publisher;
        if (beforeFirst) {
            gaps->record(static_cast<std::uint64_t>(cut));
        } else {
            publisher.afterFirstObject = [&] { gaps->record(static_cast<std::uint64_t>(cut)); };
        }
        PublishConfig cfg;
        cfg.moqEndpoint = QStringLiteral("mock://local");
        cfg.videoSource = fifo;
        cfg.fragmentDurationMs = 0;
        cfg.targetSegmentBytes = 4 * 188;
        cfg.sourceGaps = gaps;
        publisher.connect(cfg);
        moq2ts::LivePipeline pipeline;
        pipeline.start(cfg, &publisher);
        const auto deadline = std::chrono::steady_clock::now() + kWait;
        while (pipeline.running() && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        ok &= expect(!pipeline.running(), label + "the pipeline ends with the source");
        pipeline.stop();
        writer.join();
        const std::vector<PublishedObject> objects = publisher.sent(QStringLiteral("program-1"));
        if (!expect(!objects.empty(), label + "publishes")) {
            ok = false;
            continue;
        }
        int groupStarts = 0;
        int pcrs = 0;
        int marked = 0;
        bool firstPcrMarked = false;
        bool gapPcrMarked = false;
        bool gapPcrSeen = false;
        for (const PublishedObject& object : objects) {
            const QList<QByteArray> packets = packetsOf(object.payload);
            if (object.startsGroup) {
                ++groupStarts;
                ok &= expect(pidOf(packets.front()) == 0x100 && (adaptationFlags(packets.front()) & 0x40) != 0,
                             label + "Group " + std::to_string(object.groupId) + " starts at a random access point");
            }
            for (const QByteArray& packet : packets) {
                if (pidOf(packet) != 0x100) {
                    continue;
                }
                const bool discontinuity = (adaptationFlags(packet) & 0x80) != 0;
                marked += discontinuity ? 1 : 0;
                if (moq2ts::pcrOf(packet) < 0) {
                    continue;
                }
                if (pcrs++ == 0) {
                    firstPcrMarked = discontinuity;
                }
                // The first PCR of the Group after the gap: round 41's PCR
                // comes before its random access point, so round 42's.
                if (roundOf(packet) >= 41 && !gapPcrSeen) {
                    gapPcrSeen = true;
                    gapPcrMarked = discontinuity;
                }
            }
        }
        ok &= expect(groupStarts > 1, label + "several Groups");
        // The skip-ahead warning names the track, as for a multi-program
        // source, not the FIFO that SRT ingest writes into.
        QStringList skipped;
        {
            std::lock_guard<std::mutex> lock(g_warningMutex);
            skipped = g_warnings.filter(QStringLiteral("skipped ahead"));
        }
        ok &= expect(skipped.size() == 1 && skipped.front().startsWith(QStringLiteral("program-1: ")),
                     label + "the skip-ahead warning names the track: " + skipped.join(QStringLiteral(" | ")).toStdString());
        ok &= expect(catalogTrack(publisher.catalog(), QStringLiteral("program-1"))
                         .value(QStringLiteral("mpeg2tsRandomAccess"))
                         .toBool(),
                     label + "the catalog declares mpeg2tsRandomAccess");
        ok &= expect(!firstPcrMarked, label + "the first PCR of the track has no discontinuity_indicator");
        if (beforeFirst) {
            const QByteArray head = packetsOf(objects.front().payload).front();
            ok &= expect(roundOf(head) == 41, label + "the track starts at round 41 (" +
                                                  std::to_string(roundOf(head)) + ")");
            ok &= expect(marked == 0, label + "no packet is marked (" + std::to_string(marked) + ")");
        } else {
            ok &= expect(gapPcrSeen && gapPcrMarked && marked == 1,
                         label + "only the first PCR after the gap is marked (" + std::to_string(marked) + ")");
        }
    }

    // LivePipeline, SRT ingest with --per-program on a constant-rate feed (a
    // FIFO, as SRT ingest gives it): the track drops the null packets and
    // declares the source's rate in mpeg2tsMuxRate (MSFTS "Per-Program" and
    // "Mux Rate"). With --retain-null it keeps them and declares no rate. The
    // feed sends 32 packets per 0.1 s of PCR time: 481,280 b/s.
    for (const bool retainNull : {false, true}) {
        const std::string label = std::string("per-program SRT, constant-rate feed") +
                                  (retainNull ? ", --retain-null: " : ": ");
        const QByteArray data = constantRateStream(40);
        const QString fifo = dir.filePath(retainNull ? "pp-null-kept.fifo" : "pp-null-dropped.fifo");
        ::mkfifo(fifo.toLocal8Bit().constData(), 0600);
        std::thread writer([&] {
            const int fd = openWriter(fifo);
            if (fd >= 0) {
                writeAll(fd, data);
                ::close(fd);
            }
        });
        TestPublisher publisher;
        PublishConfig cfg;
        cfg.moqEndpoint = QStringLiteral("mock://local");
        cfg.videoSource = fifo;
        cfg.fragmentDurationMs = 0;
        cfg.targetSegmentBytes = 4 * 188;
        cfg.sourceGaps = std::make_shared<moq2ts::SourceGapLog>();
        cfg.retainNullPackets = retainNull;
        publisher.connect(cfg);
        moq2ts::LivePipeline pipeline;
        pipeline.start(cfg, &publisher);
        const auto deadline = std::chrono::steady_clock::now() + kWait;
        while (pipeline.running() && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        ok &= expect(!pipeline.running(), label + "the pipeline ends with the source");
        pipeline.stop();
        writer.join();
        int nulls = 0;
        int pats = 0;
        int pcrs = 0;
        for (const PublishedObject& object : publisher.sent(QStringLiteral("program-1"))) {
            for (const QByteArray& packet : packetsOf(object.payload)) {
                nulls += pidOf(packet) == 0x1FFF ? 1 : 0;
                pats += pidOf(packet) == 0 ? 1 : 0;
                pcrs += pidOf(packet) == 0x100 && moq2ts::pcrOf(packet) >= 0 ? 1 : 0;
            }
        }
        const QJsonObject track = catalogTrack(publisher.catalog(), QStringLiteral("program-1"));
        ok &= expect(track.value(QStringLiteral("mpeg2tsMode")).toString() == QStringLiteral("per-program"),
                     label + "the track is per-program");
        ok &= expect(pats > 0 && pcrs > 0, label + "the PAT and the PCR are carried");
        if (retainNull) {
            ok &= expect(nulls > 0, label + "the null packets are kept");
            ok &= expect(!track.contains(QStringLiteral("mpeg2tsMuxRate")), label + "no mpeg2tsMuxRate");
        } else {
            ok &= expect(nulls == 0, label + "no null packet is published (" + std::to_string(nulls) + ")");
            const qint64 muxRate = track.value(QStringLiteral("mpeg2tsMuxRate")).toInteger();
            ok &= expect(muxRate >= 481000 && muxRate <= 481600,
                         label + "mpeg2tsMuxRate is the source's rate: " + std::to_string(muxRate));
        }
    }

    // LivePipeline, SRT ingest with --per-program: the source PAT lists no
    // program for a while, as an encoder sends while it restarts, then lists
    // program 1 again. The track ends and a new session waits for the program
    // without a deadline, as unmodified carriage goes on; the pipeline reports
    // no error and publishes again. The empty span is 0.6, 1.5, and 2.5 s, the
    // last also without a PCR.
    struct RestartCase {
        int emptyTo;
        bool pcrStops;
    };
    for (const RestartCase& c : {RestartCase{26, false}, RestartCase{35, false}, RestartCase{45, false},
                                 RestartCase{45, true}}) {
        const std::string label = "per-program SRT, encoder restart for " + std::to_string((c.emptyTo - 20) * 100) +
                                  " ms" + (c.pcrStops ? " without a PCR: " : ": ");
        const QByteArray data = restartingStream(60, 20, c.emptyTo, c.pcrStops);
        const QString fifo = dir.filePath(QStringLiteral("pp-restart-%1-%2.fifo").arg(c.emptyTo).arg(c.pcrStops));
        ::mkfifo(fifo.toLocal8Bit().constData(), 0600);
        std::thread writer([&] {
            const int fd = openWriter(fifo);
            if (fd >= 0) {
                writeAll(fd, data);
                ::close(fd);
            }
        });
        {
            std::lock_guard<std::mutex> lock(g_warningMutex);
            g_warnings.clear();
        }
        TestPublisher publisher;
        PublishConfig cfg;
        cfg.moqEndpoint = QStringLiteral("mock://local");
        cfg.videoSource = fifo;
        cfg.fragmentDurationMs = 0;
        cfg.targetSegmentBytes = 4 * 188;
        cfg.sourceGaps = std::make_shared<moq2ts::SourceGapLog>();
        publisher.connect(cfg);
        moq2ts::LivePipeline pipeline;
        std::atomic<int> errors{0};
        QObject::connect(&pipeline, &moq2ts::LivePipeline::error, &pipeline,
                         [&errors](const QString&) { ++errors; }, Qt::DirectConnection);
        pipeline.start(cfg, &publisher);
        const auto deadline = std::chrono::steady_clock::now() + kWait;
        while (pipeline.running() && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        ok &= expect(!pipeline.running(), label + "the pipeline ends with the source");
        pipeline.stop();
        writer.join();
        // The second session's track is program-2 (LivePipeline numbers them).
        bool after = false;
        for (const PublishedObject& object : publisher.sent(QStringLiteral("program-2"))) {
            for (const QByteArray& packet : packetsOf(object.payload)) {
                after = after || (pidOf(packet) == 0x100 && moq2ts::pcrOf(packet) >= 0 && roundOf(packet) >= c.emptyTo);
            }
        }
        QStringList waits;
        {
            std::lock_guard<std::mutex> lock(g_warningMutex);
            waits = g_warnings.filter(QStringLiteral("waiting for"));
        }
        ok &= expect(errors.load() == 0, label + "no error (" + std::to_string(errors.load()) + ")");
        ok &= expect(publisher.sessions() == 2, label + "a second session (" + std::to_string(publisher.sessions()) + ")");
        ok &= expect(after, label + "publishes again after the restart");
        ok &= expect(waits.size() <= 1, label + "at most one waiting line (" + std::to_string(waits.size()) + ")");
    }

    // An interrupt while the new session waits for the program, as on SIGINT,
    // which stops the pipeline and then closes the SRT pipe, is not an error.
    {
        const std::string label = "per-program SRT, interrupt during the restart wait: ";
        const QByteArray data = restartingStream(30, 20, 30);
        const QString fifo = dir.filePath(QStringLiteral("pp-interrupt.fifo"));
        ::mkfifo(fifo.toLocal8Bit().constData(), 0600);
        std::atomic<int> writerFd{-1};
        std::thread writer([&] {
            const int fd = openWriter(fifo);
            if (fd >= 0) {
                writeAll(fd, data);
            }
            writerFd.store(fd);
        });
        {
            std::lock_guard<std::mutex> lock(g_warningMutex);
            g_warnings.clear();
        }
        TestPublisher publisher;
        PublishConfig cfg;
        cfg.moqEndpoint = QStringLiteral("mock://local");
        cfg.videoSource = fifo;
        cfg.fragmentDurationMs = 0;
        cfg.targetSegmentBytes = 4 * 188;
        cfg.sourceGaps = std::make_shared<moq2ts::SourceGapLog>();
        publisher.connect(cfg);
        moq2ts::LivePipeline pipeline;
        std::atomic<int> errors{0};
        QObject::connect(&pipeline, &moq2ts::LivePipeline::error, &pipeline,
                         [&errors](const QString&) { ++errors; }, Qt::DirectConnection);
        pipeline.start(cfg, &publisher);
        bool waiting = false;
        const auto deadline = std::chrono::steady_clock::now() + kWait;
        while (!waiting && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            std::lock_guard<std::mutex> lock(g_warningMutex);
            waiting = !g_warnings.filter(QStringLiteral("waiting for")).isEmpty();
        }
        pipeline.requestStop();
        writer.join();
        if (writerFd.load() >= 0) {
            ::close(writerFd.load());
        }
        pipeline.stop();
        ok &= expect(waiting, label + "the new session waits");
        ok &= expect(errors.load() == 0, label + "no error (" + std::to_string(errors.load()) + ")");
    }

    std::cerr << (ok ? "OK\n" : "FAILED\n");
    return ok ? 0 : 1;
}
