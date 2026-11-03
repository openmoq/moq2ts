#include "media/M2tsPacketizer.h"
#include "media/MptsPipeline.h"
#include "media/SourceGapLog.h"
#include "publish/MoqxrPublisher.h"
#include "publish/ObjectLookahead.h"
#include "ts_test_builder.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSet>
#include <QTemporaryDir>

#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <iostream>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

using moq2ts::IMoqOutput;
using moq2ts::MoqxrPublisher;
using moq2ts::MptsConfig;
using moq2ts::MptsPipeline;
using moq2ts::MptsProgramConfig;
using moq2ts::PublishConfig;
using moq2ts::PublishedObject;
using moq2ts::ReadyObject;
namespace tb = moq2ts::test;

namespace {
bool expect(bool cond, const std::string& msg) {
    if (!cond) { std::cerr << "FAIL: " << msg << '\n'; return false; }
    return true;
}

const auto kWait = std::chrono::seconds(20);

// Every message of the run: the mock publisher logs each Object it publishes.
std::mutex g_logMutex;
QStringList g_log;

void captureMessage(QtMsgType type, const QMessageLogContext&, const QString& message) {
    {
        std::lock_guard<std::mutex> lock(g_logMutex);
        g_log.append(message);
    }
    if (type != QtDebugMsg) {
        std::fprintf(stderr, "%s\n", qUtf8Printable(message));
    }
}

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

// One PAT listing every program, one PMT per program (PMT PID 0x1000 + i), and
// per program a PCR/video PID 0x100 + i. Each round of 0.1 s of PCR time
// repeats the PSI and gives every program a PCR, a random access PES packet
// with a PTS, and eight more video packets, which start with the round
// number (see roundOf). From changeRound on, the PMT of
// changeProgram is version 1 and lists an audio stream too. From dropRound
// on, dropProgram leaves the PAT (version 1) and sends no more packets.
QByteArray mptsStream(const QList<int>& programs, int rounds, int changeProgram = 0, int changeRound = -1,
                      int dropProgram = 0, int dropRound = -1) {
    int patCc = 0;
    std::vector<int> pmtCc(programs.size(), 0);
    std::vector<int> videoCc(programs.size(), 0);
    QByteArray ts;
    for (int round = 0; round < rounds; ++round) {
        const bool dropped = dropRound >= 0 && round >= dropRound;
        const auto present = [&](int i) { return !(dropped && programs[i] == dropProgram); };
        QList<std::pair<int, int>> pat;
        for (int i = 0; i < programs.size(); ++i) {
            if (present(i)) {
                pat.append({programs[i], 0x1000 + i});
            }
        }
        ts += tb::psiPacket(0, tb::patSection(pat, dropped ? 1 : 0), patCc++);
        for (int i = 0; i < programs.size(); ++i) {
            if (!present(i)) {
                continue;
            }
            const int video = 0x100 + i;
            const bool changed = programs[i] == changeProgram && round >= changeRound;
            QList<std::pair<int, int>> streams{{0x1B, video}};
            if (changed) {
                streams.append({0x0F, 0x200 + i});
            }
            ts += tb::psiPacket(0x1000 + i, tb::pmtSection(programs[i], video, streams, changed ? 1 : 0), pmtCc[i]++);
        }
        for (int i = 0; i < programs.size(); ++i) {
            if (!present(i)) {
                continue;
            }
            const int video = 0x100 + i;
            ts += pcrPacket(video, static_cast<std::int64_t>(round) * 2700000, videoCc[i]++);
            ts += tb::tsPacket(video, true, tb::pesHeaderWithPts(static_cast<std::uint64_t>(round) * 9000), videoCc[i]++, 0x40);
            for (int k = 0; k < 8; ++k) {
                QByteArray payload(184, char(k));
                payload[0] = static_cast<char>((round >> 8) & 0xFF);
                payload[1] = static_cast<char>(round & 0xFF);
                ts += tb::tsPacket(video, false, payload, videoCc[i]++);
            }
        }
    }
    return ts;
}

// Two programs of a 40 Mbit/s multiplex, mostly null packets, from 0.05 s to
// seconds: PAT and PMTs every 400 ms, a PCR every 40 ms, about 2 Mbit/s of
// video per program, and a random access point every 2 s (the first at 2 s).
// So a start-up window of 4096 packets (154 ms) misses the PSI, and one of
// 20,000 packets (0.75 s) misses the first random access point.
QByteArray fastMultiplex(double seconds) {
    constexpr double packetsPerSecond = 40e6 / (188 * 8);
    int patCc = 0;
    int pmtCc[2] = {0, 0};
    int videoCc[2] = {0, 0};
    std::int64_t nextPsi = -1;
    std::int64_t nextPcr[2] = {-1, -1};
    std::int64_t nextRap[2] = {-1, -1};
    QByteArray ts;
    for (std::int64_t slot = static_cast<std::int64_t>(0.05 * packetsPerSecond);
         slot < static_cast<std::int64_t>(seconds * packetsPerSecond); ++slot) {
        const double t = slot / packetsPerSecond;
        const std::int64_t ms = static_cast<std::int64_t>(t * 1000);
        if (nextPsi < 0) {
            nextPsi = (ms / 400 + 1) * 400;
            for (int i = 0; i < 2; ++i) {
                nextPcr[i] = (ms / 40 + 1) * 40;
                nextRap[i] = (ms / 2000 + 1) * 2000;
            }
        }
        if (ms >= nextPsi) {
            nextPsi += 400;
            ts += tb::psiPacket(0, tb::patSection({{101, 0x1000}, {102, 0x1001}}), patCc++);
            for (int i = 0; i < 2; ++i) {
                ts += tb::psiPacket(0x1000 + i, tb::pmtSection(101 + i, 0x100 + i, {{0x1B, 0x100 + i}}), pmtCc[i]++);
            }
            slot += 2;
            continue;
        }
        const int i = static_cast<int>(slot % 2);
        if (ms >= nextPcr[i]) {
            nextPcr[i] += 40;
            ts += pcrPacket(0x100 + i, static_cast<std::int64_t>(t * 27e6), videoCc[i]++);
        } else if (slot % 20 < 2 && ms >= nextRap[i]) {
            nextRap[i] += 2000;
            ts += tb::tsPacket(0x100 + i, true, tb::pesHeaderWithPts(static_cast<std::uint64_t>(t * 90000)),
                               videoCc[i]++, 0x40);
        } else if (slot % 20 < 2) {
            ts += tb::tsPacket(0x100 + i, false, QByteArray(184, char(i)), videoCc[i]++);
        } else {
            ts += tb::tsPacket(0x1FFF, false, QByteArray(184, char(0xFF)));
        }
    }
    return ts;
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

// Writes data at its 40 Mbit/s rate, as a live encoder does.
void writePaced(int fd, const QByteArray& data) {
    constexpr qsizetype chunk = 40000000 / 8 / 20 / 188 * 188;   // 50 ms
    const auto start = std::chrono::steady_clock::now();
    for (qsizetype offset = 0; offset < data.size(); offset += chunk) {
        std::this_thread::sleep_until(start + std::chrono::milliseconds(50 * (offset / chunk)));
        writeAll(fd, data.mid(offset, chunk));
    }
}

// Waits until done() holds, at most kWait. Returns done().
bool waitFor(const std::function<bool()>& done) {
    const auto deadline = std::chrono::steady_clock::now() + kWait;
    while (!done() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return done();
}

QStringList logSnapshot() {
    std::lock_guard<std::mutex> lock(g_logMutex);
    return g_log;
}

// Publishes as the mock publisher does, through the same ObjectLookahead,
// and logs the same "MOCK MSFTS publish" lines. It also keeps every Object it
// sends, and can hold back its first pull, as a relay without a subscriber
// does: moqxr asks for the first Object once a subscription exists.
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
        qDebug() << "MOCK MSFTS catalog bytes=" << catalog.size();
        if (beforeFirstPull) {
            beforeFirstPull(running);
        }
        moq2ts::ObjectLookahead lookahead(std::move(nextObject));
        while (running.load(std::memory_order_acquire)) {
            std::optional<ReadyObject> ready = lookahead.next();
            if (!ready.has_value()) {
                return true;
            }
            const PublishedObject& object = ready->object;
            QDebug line = qDebug();
            line << "MOCK MSFTS publish" << object.trackName << "group=" << object.groupId
                 << "object=" << object.objectId << "bytes=" << object.payload.size()
                 << "final=" << ready->finalInGroup;
            if (object.endsBroadcast) {
                line << "ends_broadcast";
            }
            std::lock_guard<std::mutex> lock(m_mutex);
            m_sent.push_back(std::move(*ready));
        }
        return true;
    }

    // Runs before the first pull, on the publishing thread.
    std::function<void(const std::atomic<bool>& running)> beforeFirstPull;

    std::vector<ReadyObject> sent() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_sent;
    }

private:
    bool m_connected = false;
    mutable std::mutex m_mutex;
    std::vector<ReadyObject> m_sent;
};

struct Run {
    bool finished = false;   // the pipeline stopped on its own within kWait
    QStringList errors;
    QStringList ended;   // "<program>: <reason>" for each program that ended early
    QStringList log;
    QByteArray catalog;
    std::vector<ReadyObject> sent;   // what the publisher sent, in order
};

// Publishes source to a TestPublisher with these programs. A FIFO source is
// fed from a writer thread: fifoData at once, or what feed writes to the open
// descriptor, after which the writer closes it (the end of the source).
// atLiveEdge turns true once the pipeline has started every program at the
// live edge (MptsPipeline::atLiveEdge): what the feed writes then is live.
using Feed = std::function<void(int fd, const MptsPipeline& pipeline, const std::atomic<bool>& atLiveEdge)>;

struct Options {
    std::shared_ptr<moq2ts::SourceGapLog> sourceGaps;
    std::function<void(const std::atomic<bool>& running)> beforeFirstPull;
};

Run publish(const QString& source, const QList<MptsProgramConfig>& programs, const QByteArray& fifoData,
            const Feed& feed = {}, const std::function<void(const QString&)>& onError = {},
            const Options& options = {}) {
    {
        std::lock_guard<std::mutex> lock(g_logMutex);
        g_log.clear();
    }

    PublishConfig relay;
    relay.moqEndpoint = QStringLiteral("mock://local");
    relay.namespaceName = QStringLiteral("live/mpts");
    relay.fragmentDurationMs = 0;
    MptsConfig config;
    config.sourcePath = source;
    config.programs = programs;
    config.targetSegmentBytes = 4 * 188;
    config.sourceGaps = options.sourceGaps;

    Run run;
    std::mutex errorMutex;
    TestPublisher publisher;
    publisher.beforeFirstPull = options.beforeFirstPull;
    publisher.connect(relay);
    MptsPipeline pipeline;
    std::atomic<bool> atLiveEdge{false};
    QObject::connect(&pipeline, &MptsPipeline::atLiveEdge, &pipeline, [&] { atLiveEdge = true; },
                     Qt::DirectConnection);
    QObject::connect(&pipeline, &MptsPipeline::error, &pipeline, [&](const QString& message) {
        {
            std::lock_guard<std::mutex> lock(errorMutex);
            run.errors.append(message);
        }
        if (onError) {
            onError(message);
        }
    }, Qt::DirectConnection);
    // onError hears why a program ended too.
    QObject::connect(&pipeline, &MptsPipeline::programEnded, &pipeline, [&](int number, const QString& reason) {
        {
            std::lock_guard<std::mutex> lock(errorMutex);
            run.ended.append(QStringLiteral("%1: %2").arg(number).arg(reason));
        }
        if (onError) {
            onError(reason);
        }
    }, Qt::DirectConnection);
    std::thread writer;
    if (!fifoData.isEmpty() || feed) {
        ::mkfifo(source.toLocal8Bit().constData(), 0600);
        writer = std::thread([&] {
            const int fd = openWriter(source);
            if (fd < 0) {
                return;
            }
            if (feed) {
                feed(fd, pipeline, atLiveEdge);
            } else {
                writeAll(fd, fifoData);
            }
            ::close(fd);
        });
    }
    pipeline.start(relay, config, &publisher);
    const auto deadline = std::chrono::steady_clock::now() + kWait;
    while (pipeline.running() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    run.finished = !pipeline.running();
    pipeline.stop();
    if (writer.joinable()) {
        writer.join();
    }
    run.catalog = pipeline.catalogJson();
    run.sent = publisher.sent();
    std::lock_guard<std::mutex> lock(g_logMutex);
    run.log = g_log;
    return run;
}

// A live source: the lead, which open() reads to build the catalog, before
// the publisher's first pull, and the rest once the pipeline has started every
// program at the live edge, so that the first pull does not drop it.
Feed liveFeed(const QByteArray& data, qsizetype leadBytes) {
    return [data, leadBytes](int fd, const MptsPipeline&, const std::atomic<bool>& atLiveEdge) {
        writeAll(fd, data.left(leadBytes));
        if (waitFor([&] { return atLiveEdge.load(); })) {
            writeAll(fd, data.mid(leadBytes));
        }
    };
}

// The size of the first rounds of mptsStream(programs, ...).
qsizetype roundsBytes(int programs, int rounds) {
    return static_cast<qsizetype>(rounds) * (1 + programs + programs * 10) * 188;
}

MptsProgramConfig program(int number, qint64 muxRateBps = 0) {
    MptsProgramConfig config;
    config.programNumber = number;
    config.muxRateBps = muxRateBps;
    return config;
}

const QRegularExpression kPublishLine(QStringLiteral("^MOCK MSFTS publish \"([^\"]+)\" group="));

// The track of each Object the mock published, in order.
QStringList publishedTracks(const QStringList& log) {
    QStringList tracks;
    for (const QString& line : log) {
        const QRegularExpressionMatch match = kPublishLine.match(line);
        if (match.hasMatch()) {
            tracks.append(match.captured(1));
        }
    }
    return tracks;
}

QStringList catalogTracks(const QByteArray& catalog) {
    QStringList names;
    for (const QJsonValue& track : QJsonDocument::fromJson(catalog).object().value(QStringLiteral("tracks")).toArray()) {
        names.append(track.toObject().value(QStringLiteral("name")).toString());
    }
    return names;
}

QJsonObject catalogTrack(const QByteArray& catalog, const QString& name) {
    for (const QJsonValue& track : QJsonDocument::fromJson(catalog).object().value(QStringLiteral("tracks")).toArray()) {
        if (track.toObject().value(QStringLiteral("name")).toString() == name) {
            return track.toObject();
        }
    }
    return {};
}

// The initData of a track: its initDataList entry, decoded.
QByteArray initDataOf(const QByteArray& catalog, const QString& track) {
    const QJsonArray list = QJsonDocument::fromJson(catalog).object().value(QStringLiteral("initDataList")).toArray();
    for (const QJsonValue& entry : list) {
        if (entry.toObject().value(QStringLiteral("id")).toString() == QStringLiteral("init-") + track) {
            return QByteArray::fromBase64(entry.toObject().value(QStringLiteral("data")).toString().toLatin1());
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

// The mptsStream round of a packet on a video PID: from its PCR, its PTS, or
// the round number that starts its payload.
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

// The Objects sent on one track, in order.
std::vector<ReadyObject> trackObjects(const std::vector<ReadyObject>& sent, const QString& track) {
    std::vector<ReadyObject> objects;
    for (const ReadyObject& ready : sent) {
        if (ready.object.trackName == track) {
            objects.push_back(ready);
        }
    }
    return objects;
}

// The index of the first Object that carries a packet of round or later on
// pid, or -1.
qsizetype firstObjectFrom(const std::vector<ReadyObject>& objects, int pid, int round) {
    for (std::size_t index = 0; index < objects.size(); ++index) {
        for (const QByteArray& packet : packetsOf(objects[index].object.payload)) {
            if (pidOf(packet) == pid && roundOf(packet) >= round) {
                return static_cast<qsizetype>(index);
            }
        }
    }
    return -1;
}

std::string joined(const QStringList& list) {
    return list.join(QStringLiteral(" | ")).toStdString();
}
}  // namespace

int main() {
    std::signal(SIGPIPE, SIG_IGN);
    qInstallMessageHandler(captureMessage);
    bool ok = true;
    QTemporaryDir dir;
    ok &= expect(dir.isValid(), "temporary directory");

    // Two programs: both tracks publish Objects, catalog lists four tracks.
    {
        const Run run = publish(dir.filePath("two.fifo"), {program(101, 6000000), program(102)}, QByteArray(),
                                liveFeed(mptsStream({101, 102}, 60), roundsBytes(2, 15)));
        ok &= expect(run.finished, "two programs: the pipeline ends with the source");
        ok &= expect(run.errors.isEmpty(), "two programs: no error: " + joined(run.errors));
        const QStringList tracks = publishedTracks(run.log);
        for (const QString& track : {QStringLiteral("program-101"), QStringLiteral("program-102"),
                                     QStringLiteral("program-101.timeline"), QStringLiteral("program-102.timeline")}) {
            ok &= expect(tracks.count(track) > 0, "two programs: " + track.toStdString() + " publishes Objects");
        }
        ok &= expect(catalogTracks(run.catalog) == QStringList({"program-101", "program-101.timeline", "program-102",
                                                                "program-102.timeline"}),
                     "two programs: catalog lists four tracks: " + run.catalog.toStdString());
        // The source ended cleanly: its last Object is the closing catalog
        // of MSF -01 Section 11.3, after every media Object, and only one.
        ok &= expect(tracks.count(QStringLiteral("catalog")) == 1, "two programs: one closing catalog");
        ok &= expect(!run.log.isEmpty() && run.log.last().contains(QStringLiteral("publish \"catalog\"")) &&
                         run.log.last().contains(QStringLiteral("ends_broadcast")),
                     "two programs: the closing catalog is the last Object");
        const QJsonObject first = catalogTrack(run.catalog, QStringLiteral("program-101"));
        const QJsonObject second = catalogTrack(run.catalog, QStringLiteral("program-102"));
        ok &= expect(first.value(QStringLiteral("mpeg2tsMode")).toString() == QStringLiteral("per-program"),
                     "two programs: per-program mode");
        ok &= expect(first.value(QStringLiteral("mpeg2tsMuxRate")).toDouble() == 6000000,
                     "two programs: program 101 declares its configured mux rate");
        ok &= expect(second.value(QStringLiteral("mpeg2tsMuxRate")).toDouble() > 0,
                     "two programs: program 102 declares a worked-out mux rate");
        ok &= expect(second.value(QStringLiteral("mpeg2tsProgramNumber")).toInt() == 102,
                     "two programs: program 102 is named");
    }

    // MSFTS 6.7: a configured mux rate below the measured rate of the program
    // makes the receiver overflow, so it is warned about; one above is not.
    {
        const Run run = publish(dir.filePath("low-rate.fifo"), {program(101, 6000000), program(102, 10000)},
                                QByteArray(), liveFeed(mptsStream({101, 102}, 30), roundsBytes(2, 15)));
        ok &= expect(run.errors.isEmpty(), "low mux rate: no error: " + joined(run.errors));
        const QStringList below = run.log.filter(QStringLiteral("below the rate measured"));
        ok &= expect(below.size() == 1 && below.front().contains(QLatin1String("program 102")),
                     "low mux rate: one warning, for program 102: " + joined(below));
    }

    // Program 103 missing from the PAT: start fails with an error naming 103.
    {
        const Run run = publish(dir.filePath("missing.fifo"), {program(101), program(102), program(103)},
                                mptsStream({101, 102}, 60));
        ok &= expect(run.finished, "missing program: the pipeline stops");
        ok &= expect(run.errors.contains(QStringLiteral("program 103 is not in the source PAT")),
                     "missing program: the error names 103: " + joined(run.errors));
        ok &= expect(publishedTracks(run.log).isEmpty(), "missing program: nothing is published");
    }

    // Program 101's PMT changes: one new catalog Object, and program 102 keeps
    // publishing after it. The writer holds back each part of the stream until
    // the pipeline has handled the one before, so the order is causal: rounds
    // 0-29 (version 0), then rounds 30-34 (program 101's PMT at version 1) once
    // the publisher pulls, then the rest once the new catalog went out.
    {
        const QByteArray data = mptsStream({101, 102}, 60, 101, 30);
        const qsizetype roundBytes = (1 + 2 + 2 * 10) * 188;
        QByteArray initialCatalog;
        bool catalogSeen = false;
        qsizetype program102BeforeChange = -1;
        const Feed feed = [&](int fd, const MptsPipeline& pipeline, const std::atomic<bool>& atLiveEdge) {
            writeAll(fd, data.left(30 * roundBytes));
            if (!waitFor([&] { return atLiveEdge.load(); })) {
                return;
            }
            initialCatalog = pipeline.catalogJson();
            writeAll(fd, data.mid(30 * roundBytes, 5 * roundBytes));
            catalogSeen = waitFor([] { return publishedTracks(logSnapshot()).contains(QStringLiteral("catalog")); });
            program102BeforeChange = publishedTracks(logSnapshot()).count(QStringLiteral("program-102"));
            writeAll(fd, data.mid(35 * roundBytes));
        };
        const Run run = publish(dir.filePath("pmt.fifo"), {program(101), program(102)}, QByteArray(), feed);
        ok &= expect(run.finished, "PMT change: the pipeline ends with the source");
        ok &= expect(run.errors.isEmpty(), "PMT change: no error: " + joined(run.errors));
        ok &= expect(catalogSeen, "PMT change: the new catalog goes out");
        const QStringList tracks = publishedTracks(run.log);
        // The new catalog, then the closing one when the source ends.
        ok &= expect(tracks.count(QStringLiteral("catalog")) == 2, "PMT change: one new catalog Object");
        ok &= expect(tracks.count(QStringLiteral("program-102")) > program102BeforeChange,
                     "PMT change: program 102 keeps publishing after the new catalog");
        ok &= expect(catalogTracks(run.catalog).size() == 4, "PMT change: the new catalog lists every track");
        const QByteArray changedPmt = tb::pmtSection(101, 0x100, {{0x1B, 0x100}, {0x0F, 0x200}}, 1);
        ok &= expect(initDataOf(run.catalog, QStringLiteral("program-101")).contains(changedPmt),
                     "PMT change: program 101's initData carries its version 1 PMT");
        ok &= expect(!initDataOf(initialCatalog, QStringLiteral("program-102")).isEmpty() &&
                         initDataOf(run.catalog, QStringLiteral("program-102")) ==
                             initDataOf(initialCatalog, QStringLiteral("program-102")),
                     "PMT change: program 102's initData is unchanged");
    }

    // Program 101 leaves the PAT while 102 goes on: 101's last Object closes
    // its Group at once, not when the session ends. The writer sends rounds
    // 0-14, then rounds 15-34 once the publisher pulls (101 leaves at round
    // 30), waits for the error that says 101's track ended, which comes after
    // its end is queued, then sends rounds 35-59. So program 102's Objects of
    // those rounds come after 101's end, whatever the scheduling.
    {
        const auto stream = [](int rounds) { return mptsStream({101, 102}, rounds, 0, -1, 101, 30); };
        const QByteArray data = stream(60);
        const qsizetype split = stream(35).size();
        std::atomic<bool> ended{false};
        const Feed feed = [&](int fd, const MptsPipeline&, const std::atomic<bool>& atLiveEdge) {
            writeAll(fd, data.left(roundsBytes(2, 15)));
            if (!waitFor([&] { return atLiveEdge.load(); })) {
                return;
            }
            writeAll(fd, data.mid(roundsBytes(2, 15), split - roundsBytes(2, 15)));
            if (waitFor([&] { return ended.load(); })) {
                writeAll(fd, data.mid(split));
            }
        };
        const Run run = publish(dir.filePath("drop.fifo"), {program(101), program(102)}, QByteArray(), feed,
                                [&](const QString& message) {
                                    if (message.contains(QLatin1String("Program 101 left the source PAT"))) {
                                        ended = true;
                                    }
                                });
        ok &= expect(run.finished, "program ends: the pipeline ends with the source");
        ok &= expect(run.errors.isEmpty(), "program ends: no pipeline error: " + joined(run.errors));
        ok &= expect(run.ended == QStringList({"101: Program 101 left the source PAT; the track ends."}),
                     "program ends: programEnded says which and why: " + joined(run.ended));
        // A program that ended on an error makes no commitment that no content
        // follows: no closing catalog.
        ok &= expect(publishedTracks(run.log).count(QStringLiteral("catalog")) == 0,
                     "program ends: no closing catalog after an error");
        qsizetype last101 = -1;
        qsizetype after = 0;
        for (qsizetype i = 0; i < run.log.size(); ++i) {
            if (run.log[i].startsWith(QLatin1String("MOCK MSFTS publish \"program-101\""))) {
                last101 = i;
                after = 0;
            } else if (last101 >= 0 && run.log[i].startsWith(QLatin1String("MOCK MSFTS publish \"program-102\""))) {
                ++after;
            }
        }
        ok &= expect(last101 >= 0 && run.log[last101].endsWith(QLatin1String("final= true")),
                     "program ends: 101's last Object closes its Group");
        // Rounds 35-59 give program 102 about 100 Objects after it; without
        // the end marker, 101's last Object waits for the end of the source.
        ok &= expect(after >= 10, "program ends: 101's last Object goes out before program 102's later Objects (" +
                                      std::to_string(after) + " after it)");
    }

    // The first subscriber comes 30 s into the source: the publisher pulls
    // first after rounds 0-314. Nothing queued before that pull goes out:
    // each program's first Object starts at its next random access point
    // after it. Its first PCR follows no published PCR, so it has no
    // discontinuity_indicator (MSFTS 5.4: only between consecutive Groups).
    {
        const QByteArray data = mptsStream({101, 102}, 375);
        const qsizetype prePull = roundsBytes(2, 315);
        std::atomic<bool> release{false};
        Options options;
        options.beforeFirstPull = [&](const std::atomic<bool>& running) {
            const auto deadline = std::chrono::steady_clock::now() + kWait;
            while (!release && running && std::chrono::steady_clock::now() < deadline) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        };
        const Feed feed = [&](int fd, const MptsPipeline&, const std::atomic<bool>& atLiveEdge) {
            writeAll(fd, data.left(prePull));
            // The fan-out reads the source as it comes, whatever waits after
            // it: the pull comes once the FIFO holds nothing more.
            waitFor([&] {
                int queued = 0;
                return ::ioctl(fd, FIONREAD, &queued) == 0 && queued == 0;
            });
            release = true;
            if (waitFor([&] { return atLiveEdge.load(); })) {
                writeAll(fd, data.mid(prePull));
            }
        };
        const Run run = publish(dir.filePath("late.fifo"), {program(101), program(102)}, QByteArray(), feed, {},
                                options);
        ok &= expect(run.finished, "late subscriber: the pipeline ends with the source");
        ok &= expect(run.errors.isEmpty(), "late subscriber: no error: " + joined(run.errors));
        for (int i = 0; i < 2; ++i) {
            const QString track = QStringLiteral("program-%1").arg(101 + i);
            const std::string label = "late subscriber: " + track.toStdString() + ": ";
            const int pid = 0x100 + i;
            const std::vector<ReadyObject> objects = trackObjects(run.sent, track);
            if (!expect(!objects.empty(), label + "publishes")) {
                ok = false;
                continue;
            }
            const QByteArray head = objects.front().object.payload.left(188);
            const int round = pidOf(head) == pid ? roundOf(head) : -1;
            ok &= expect(objects.front().object.startsGroup && (adaptationFlags(head) & 0x40) != 0 &&
                             round >= 315 && round <= 345,
                         label + "the first Object starts at a random access point after the pull (round " +
                             std::to_string(round) + ", pulled after round 314)");
            std::optional<bool> marked;
            for (const ReadyObject& ready : objects) {
                for (const QByteArray& packet : packetsOf(ready.object.payload)) {
                    if (!marked.has_value() && pidOf(packet) == pid && moq2ts::pcrOf(packet) >= 0) {
                        marked = (adaptationFlags(packet) & 0x80) != 0;
                    }
                }
            }
            ok &= expect(marked.has_value() && !*marked, label + "the first PCR has no discontinuity_indicator");
        }
    }

    // The source skips ahead mid-GOP in every program: it stops in round 30
    // after program 102's random access point and four more of its packets,
    // and resumes in round 40 right after program 102's random access point.
    // Each program drops what comes before its next random access point
    // (round 41), so its next Group starts with one, and its Object before
    // the gap closes its Group.
    {
        const QByteArray full = mptsStream({101, 102}, 70);
        const qsizetype cut = roundsBytes(2, 30) + (3 + 10 + 2 + 4) * 188;
        const qsizetype resume = roundsBytes(2, 40) + (3 + 10 + 2) * 188;
        const QByteArray data = full.left(cut) + full.mid(resume);
        Options options;
        options.sourceGaps = std::make_shared<moq2ts::SourceGapLog>();
        options.sourceGaps->record(static_cast<std::uint64_t>(cut));
        const Run run = publish(dir.filePath("gap.fifo"), {program(101), program(102)}, QByteArray(),
                                liveFeed(data, roundsBytes(2, 15)), {}, options);
        ok &= expect(run.finished, "gap: the pipeline ends with the source");
        ok &= expect(run.errors.isEmpty(), "gap: no error: " + joined(run.errors));
        // Both programs warn of the gap, each naming its track.
        const QStringList skipped = run.log.filter(QStringLiteral("skipped ahead"));
        ok &= expect(skipped.size() == 2 && skipped.filter(QRegularExpression(QStringLiteral("^program-101: "))).size() == 1 &&
                         skipped.filter(QRegularExpression(QStringLiteral("^program-102: "))).size() == 1,
                     "gap: each program's warning names its track: " + joined(skipped));
        for (int i = 0; i < 2; ++i) {
            const QString track = QStringLiteral("program-%1").arg(101 + i);
            const std::string label = "gap: " + track.toStdString() + ": ";
            const int pid = 0x100 + i;
            const std::vector<ReadyObject> objects = trackObjects(run.sent, track);
            const qsizetype after = firstObjectFrom(objects, pid, 40);
            if (!expect(after > 0, label + "Objects before and after the gap")) {
                ok = false;
                continue;
            }
            const ReadyObject& first = objects[after];
            const ReadyObject& before = objects[after - 1];
            ok &= expect(first.object.startsGroup && first.object.groupId != before.object.groupId,
                         label + "the first Object after the gap starts a Group");
            const QByteArray head = first.object.payload.left(188);
            ok &= expect(pidOf(head) == pid && (adaptationFlags(head) & 0x40) != 0 && roundOf(head) == 41,
                         label + "that Group starts with the random access point of round 41 (round " +
                             std::to_string(roundOf(head)) + ")");
            ok &= expect(before.finalInGroup, label + "the Object before the gap closes its Group");
            // MSFTS 5.4: the first packet that carries a PCR in the Group after
            // the gap has discontinuity_indicator set. Every other packet of
            // the program goes out as the source sent it, the first PCR of
            // the track too: no published PCR precedes it.
            QSet<QByteArray> source;
            for (const QByteArray& packet : packetsOf(full)) {
                if (pidOf(packet) == pid) {
                    source.insert(packet);
                }
            }
            bool firstPcrSeen = false;
            bool awaitingGapPcr = false;
            int marked = 0;
            int changed = 0;
            for (qsizetype index = 0; index < static_cast<qsizetype>(objects.size()); ++index) {
                awaitingGapPcr = awaitingGapPcr || index == after;
                for (const QByteArray& packet : packetsOf(objects[index].object.payload)) {
                    if (pidOf(packet) != pid) {
                        continue;
                    }
                    const bool pcr = moq2ts::pcrOf(packet) >= 0;
                    const bool discontinuity = (adaptationFlags(packet) & 0x80) != 0;
                    QByteArray unmarked = packet;
                    unmarked[5] = static_cast<char>(static_cast<unsigned char>(unmarked[5]) & ~0x80);
                    // Only the bit may change.
                    changed += source.contains(discontinuity ? unmarked : packet) ? 0 : 1;
                    if (pcr && awaitingGapPcr) {
                        ok &= expect(discontinuity, label + "the first PCR after the gap has discontinuity_indicator");
                        marked += discontinuity ? 1 : 0;
                        awaitingGapPcr = false;
                        firstPcrSeen = true;
                    } else if (pcr && !firstPcrSeen) {
                        ok &= expect(!discontinuity, label + "the first PCR of the track has no discontinuity_indicator");
                        firstPcrSeen = true;
                    } else if (discontinuity) {
                        ++changed;
                    }
                }
            }
            ok &= expect(marked == 1, label + "one PCR after the gap is marked");
            ok &= expect(changed == 0, label + "the other packets are unchanged (" + std::to_string(changed) +
                                           " changed)");
            ok &= expect(catalogTrack(run.catalog, track).value(QStringLiteral("mpeg2tsRandomAccess")).toBool(),
                         label + "the catalog declares mpeg2tsRandomAccess");
        }
    }

    // A 40 Mbit/s multiplex, joined mid-stream, with its PSI every 400 ms and
    // a 2 s GOP: the start-up windows are PCR time (1 s for the PSI, 10 s for
    // the first random access point), not a packet count, so both programs
    // start and declare mpeg2tsRandomAccess.
    {
        const QByteArray data = fastMultiplex(7.0);
        const Feed feed = [&](int fd, const MptsPipeline&, const std::atomic<bool>&) { writePaced(fd, data); };
        const Run run = publish(dir.filePath("fast.fifo"), {program(101), program(102)}, QByteArray(), feed);
        ok &= expect(run.finished, "fast multiplex: the pipeline ends with the source");
        ok &= expect(run.errors.isEmpty(), "fast multiplex: no error: " + joined(run.errors));
        const QStringList tracks = publishedTracks(run.log);
        for (const QString& track : {QStringLiteral("program-101"), QStringLiteral("program-102")}) {
            ok &= expect(tracks.contains(track), "fast multiplex: " + track.toStdString() + " publishes");
            ok &= expect(catalogTrack(run.catalog, track).value(QStringLiteral("mpeg2tsRandomAccess")).toBool(),
                         "fast multiplex: " + track.toStdString() + " declares mpeg2tsRandomAccess");
            const std::vector<ReadyObject> objects = trackObjects(run.sent, track);
            const QByteArray head = objects.empty() ? QByteArray() : objects.front().object.payload.left(188);
            ok &= expect(head.size() == 188 && objects.front().object.startsGroup &&
                             pidOf(head) == (track.endsWith(QLatin1String("101")) ? 0x100 : 0x101) &&
                             (adaptationFlags(head) & 0x40) != 0,
                         "fast multiplex: " + track.toStdString() + "'s first Group starts at a random access point");
        }
    }

    // Ten programs all publish.
    {
        QList<int> numbers;
        QList<MptsProgramConfig> programs;
        for (int number = 101; number <= 110; ++number) {
            numbers.append(number);
            programs.append(program(number));
        }
        const Run run = publish(dir.filePath("ten.fifo"), programs, QByteArray(),
                                liveFeed(mptsStream(numbers, 30), roundsBytes(10, 15)));
        ok &= expect(run.finished, "ten programs: the pipeline ends with the source");
        ok &= expect(run.errors.isEmpty(), "ten programs: no error: " + joined(run.errors));
        const QStringList tracks = publishedTracks(run.log);
        for (const int number : numbers) {
            ok &= expect(tracks.count(QStringLiteral("program-%1").arg(number)) > 0,
                         "ten programs: program " + std::to_string(number) + " publishes");
        }
        ok &= expect(catalogTracks(run.catalog).size() == 20, "ten programs: the catalog lists twenty tracks");
    }

    // A seekable file is refused.
    {
        const QString path = dir.filePath("source.ts");
        QFile file(path);
        ok &= expect(file.open(QIODevice::WriteOnly) && file.write(mptsStream({101, 102}, 20)) > 0, "write file");
        file.close();
        const Run run = publish(path, {program(101), program(102)}, QByteArray());
        ok &= expect(run.finished, "file: the pipeline stops");
        ok &= expect(run.errors == QStringList({"multi-program publishing needs a live source (FIFO, pipe, or SRT)"}),
                     "file: refused with the live-source error: " + joined(run.errors));
        ok &= expect(publishedTracks(run.log).isEmpty(), "file: nothing is published");
    }

    // stop() returns promptly: on a FIFO no writer opened, and with the
    // workers blocked on a source that went silent.
    for (const bool silentWriter : {false, true}) {
        const std::string label = silentWriter ? "stop, silent source: " : "stop, no writer: ";
        const QString path = dir.filePath(silentWriter ? "silent.fifo" : "nowriter.fifo");
        ::mkfifo(path.toLocal8Bit().constData(), 0600);
        int writeFd = -1;
        bool wrote = !silentWriter;
        std::thread writer;
        if (silentWriter) {
            // Writes the start of the stream and keeps the FIFO open.
            writer = std::thread([&] {
                const auto deadline = std::chrono::steady_clock::now() + kWait;
                while (writeFd < 0 && std::chrono::steady_clock::now() < deadline) {
                    writeFd = ::open(path.toLocal8Bit().constData(), O_WRONLY | O_NONBLOCK);
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                }
                if (writeFd >= 0) {
                    ::fcntl(writeFd, F_SETFL, ::fcntl(writeFd, F_GETFL) & ~O_NONBLOCK);
                    // 12 rounds (52 KB) fit in the 64 KB pipe buffer, so
                    // the write completes whatever the reader does.
                    const QByteArray data = mptsStream({101, 102}, 12);
                    wrote = ::write(writeFd, data.constData(), data.size()) == data.size();
                }
            });
        }
        PublishConfig relay;
        relay.moqEndpoint = QStringLiteral("mock://local");
        relay.fragmentDurationMs = 0;
        MptsConfig config;
        config.sourcePath = path;
        config.programs = {program(101), program(102)};
        MoqxrPublisher publisher;
        publisher.connect(relay);
        MptsPipeline pipeline;
        pipeline.start(relay, config, &publisher);
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        ok &= expect(pipeline.running(), label + "running before stop");
        const auto begin = std::chrono::steady_clock::now();
        pipeline.stop();
        ok &= expect(std::chrono::steady_clock::now() - begin < std::chrono::seconds(3), label + "stop returns promptly");
        ok &= expect(!pipeline.running(), label + "not running after stop");
        if (writer.joinable()) {
            writer.join();
        }
        ok &= expect(wrote, label + "write the start of the stream");
        if (writeFd >= 0) {
            ::close(writeFd);
        }
    }

    std::cerr << (ok ? "OK\n" : "FAILED\n");
    return ok ? 0 : 1;
}
