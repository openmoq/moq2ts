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

    std::cerr << (ok ? "OK\n" : "FAILED\n");
    return ok ? 0 : 1;
}
