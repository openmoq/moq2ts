#include "app/PublishConfig.h"
#include "media/LivePipeline.h"
#include "publish/MoqxrPublisher.h"
#include "publish/RedundantPublisher.h"
#include "ts_test_builder.h"

#include <QCoreApplication>
#include <QTemporaryDir>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <functional>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

namespace tb = moq2ts::test;
using moq2ts::PublishConfig;
using moq2ts::PublishedObject;

namespace {
bool expect(bool cond, const std::string& msg) {
    if (!cond) { std::cerr << "FAIL: " << msg << '\n'; return false; }
    return true;
}

bool waitFor(const std::function<bool()>& cond) {
    for (int i = 0; i < 400; ++i) {
        if (cond()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return cond();
}

// Takes the pipeline's source as the real publisher does, and keeps what it
// hands out.
class Recorder final : public moq2ts::IMoqOutput {
public:
    bool connect(const PublishConfig&) override { return true; }
    bool publishLiveObjects(const PublishConfig&, const QString&, const QStringList&, const QByteArray&,
                            std::function<std::optional<PublishedObject>()> nextObject,
                            std::atomic<bool>&) override {
        while (std::optional<PublishedObject> object = nextObject()) {
            std::lock_guard lock(m_mutex);
            if (object->idle) {
                ++m_idles;
            } else {
                m_objects.push_back(*object);
            }
        }
        ended = true;
        return true;
    }
    void stop() override {}
    bool connected() const override { return true; }

    int idles() const {
        std::lock_guard lock(m_mutex);
        return m_idles;
    }
    // The media Objects, without the closing catalog.
    std::vector<PublishedObject> media() const {
        std::lock_guard lock(m_mutex);
        std::vector<PublishedObject> media;
        for (const PublishedObject& object : m_objects) {
            if (object.trackName == QStringLiteral("program-1")) {
                media.push_back(object);
            }
        }
        return media;
    }
    qsizetype mediaBytes() const {
        qsizetype bytes = 0;
        for (const PublishedObject& object : media()) {
            bytes += object.payload.size();
        }
        return bytes;
    }
    int closings() const {
        std::lock_guard lock(m_mutex);
        int closings = 0;
        for (const PublishedObject& object : m_objects) {
            closings += object.endsBroadcast ? 1 : 0;
        }
        return closings;
    }

    std::atomic<bool> ended{false};

private:
    mutable std::mutex m_mutex;
    std::vector<PublishedObject> m_objects;
    int m_idles = 0;
};

// A packet of the video PID that carries only a PCR.
QByteArray pcrPacket(std::int64_t seconds90k) {
    QByteArray packet(188, char(0xFF));
    packet[0] = 0x47;
    packet[1] = 0x01;            // PID 0x100
    packet[2] = 0x00;
    packet[3] = 0x20;            // adaptation field only
    packet[4] = static_cast<char>(183);
    packet[5] = 0x10;            // PCR_flag
    for (int i = 0; i < 4; ++i) {
        packet[6 + i] = static_cast<char>((seconds90k >> (25 - 8 * i)) & 0xFF);
    }
    packet[10] = static_cast<char>(((seconds90k & 1) << 7) | 0x7E);
    packet[11] = 0x00;
    return packet;
}

// One program whose video starts with a random access point. Its PCRs span
// over 1 s, which opening a live source reads before it returns.
QByteArray start() {
    QByteArray ts = tb::psiPacket(0x0000, tb::patSection({{1, 0x1000}}));
    ts += tb::psiPacket(0x1000, tb::pmtSection(1, 0x100, {{0x1B, 0x100}}));
    ts += pcrPacket(90000);
    ts += tb::tsPacket(0x100, true, tb::pesHeaderWithPts(9000), 0, 0x40);
    for (int cc = 1; cc < 8; ++cc) {
        ts += tb::tsPacket(0x100, false, QByteArray(184, char(0)), cc);
    }
    ts += pcrPacket(90000 + 108000);   // 1.2 s later
    return ts;
}

// The live track drops what comes before the first random access point: the
// PAT, the PMT and the first PCR.
qsizetype carriedBytes() {
    return start().size() - 3 * 188;
}

// Video packets that go on with the Group, an odd number so that the input
// pauses with an Object part filled.
QByteArray more() {
    QByteArray ts;
    for (int cc = 8; cc < 13; ++cc) {
        ts += tb::tsPacket(0x100, false, QByteArray(184, char(0)), cc % 16);
    }
    return ts;
}

struct Run {
    QTemporaryDir dir;
    Recorder recorder;
    moq2ts::LivePipeline pipeline;
    int writer = -1;

    bool begin() {
        const QByteArray path = dir.filePath(QStringLiteral("live.ts")).toLocal8Bit();
        if (::mkfifo(path.constData(), 0600) != 0) {
            return false;
        }
        PublishConfig cfg;
        cfg.moqEndpoint = QStringLiteral("mock://local");
        cfg.videoSource = QString::fromLocal8Bit(path);
        cfg.transparentMode = true;
        cfg.targetSegmentBytes = 2 * 188;
        pipeline.start(cfg, &recorder);
        writer = ::open(path.constData(), O_WRONLY);   // waits for the pipeline
        return writer >= 0;
    }
    bool write(const QByteArray& data) { return ::write(writer, data.constData(), data.size()) == data.size(); }
    void closeInput() {
        ::close(writer);
        writer = -1;
    }
    ~Run() {
        if (writer >= 0) {
            ::close(writer);
        }
        pipeline.stop();
    }
};
}  // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    std::signal(SIGPIPE, SIG_IGN);
    bool ok = true;

    // A quiet input: what has arrived goes out, then the source says it has
    // nothing yet instead of blocking. It goes on when input returns, and the
    // end of the input ends the broadcast once.
    {
        Run run;
        ok &= expect(run.begin(), "quiet: the pipeline reads the FIFO");
        ok &= expect(run.write(start()), "quiet: write the start");
        ok &= expect(waitFor([&] { return run.recorder.mediaBytes() == carriedBytes(); }),
                     "quiet: every packet that arrived goes out");
        ok &= expect(waitFor([&] { return run.recorder.idles() > 1; }), "quiet: the source reports nothing yet");

        ok &= expect(run.write(more()), "stall: write more");
        const qsizetype all = carriedBytes() + more().size();
        ok &= expect(waitFor([&] { return run.recorder.mediaBytes() == all; }),
                     "stall: the part-filled Object goes out");
        const int idles = run.recorder.idles();
        ok &= expect(waitFor([&] { return run.recorder.idles() > idles; }), "stall: then nothing yet again");
        const auto media = run.recorder.media();
        bool increasing = true;
        for (std::size_t i = 1; i < media.size(); ++i) {
            increasing &= media[i].groupId == media[i - 1].groupId && media[i].objectId == media[i - 1].objectId + 1;
        }
        ok &= expect(increasing, "stall: the Object IDs go on");

        run.closeInput();
        ok &= expect(waitFor([&] { return run.recorder.ended.load(); }), "end: the source ends");
        ok &= expect(run.recorder.closings() == 1, "end: one closing catalog");
    }

    // A stop request while the input is quiet ends the source promptly.
    {
        Run run;
        ok &= expect(run.begin(), "stop: the pipeline reads the FIFO");
        ok &= expect(run.write(start()), "stop: write the start");
        ok &= expect(waitFor([&] { return run.recorder.idles() > 0; }), "stop: the source reports nothing yet");
        const auto asked = std::chrono::steady_clock::now();
        run.pipeline.requestStop();
        ok &= expect(waitFor([&] { return run.recorder.ended.load(); }), "stop: the source ends");
        ok &= expect(std::chrono::steady_clock::now() - asked < std::chrono::seconds(1), "stop: within 1 s");
    }

    // groupOpened: once per Group of a media track, never for the catalog or a
    // timeline.
    {
        PublishConfig cfg;
        cfg.moqEndpoint = QStringLiteral("mock://local");
        cfg.fragmentDurationMs = 0;
        moq2ts::MoqxrPublisher publisher;
        QStringList opened;
        QObject::connect(&publisher, &moq2ts::MoqxrPublisher::groupOpened, &publisher,
                         [&opened](const QString& track, qulonglong group) {
                             opened << QStringLiteral("%1/%2").arg(track).arg(group);
                         },
                         Qt::DirectConnection);
        std::vector<PublishedObject> script;
        for (const auto& [track, group, id] : {std::tuple{"program-1", 0, 0}, std::tuple{"program-1.timeline", 0, 0},
                                               std::tuple{"program-1", 0, 1}, std::tuple{"program-1", 1, 0},
                                               std::tuple{"program-1.timeline", 1, 0}}) {
            PublishedObject object;
            object.trackName = QString::fromLatin1(track);
            object.groupId = static_cast<std::uint64_t>(group);
            object.objectId = static_cast<std::uint64_t>(id);
            object.payload = QByteArray(188, 'x');
            object.finalInGroup = object.trackName.endsWith(QStringLiteral(".timeline"));
            script.push_back(object);
        }
        std::size_t next = 0;
        std::atomic<bool> running{true};
        ok &= expect(publisher.connect(cfg), "groups: connect");
        publisher.publishLiveObjects(cfg, QStringLiteral("program-1"), {QStringLiteral("program-1.timeline")},
                                     QByteArray("{}"),
                                     [&]() -> std::optional<PublishedObject> {
                                         if (next == script.size()) {
                                             return std::nullopt;
                                         }
                                         return script[next++];
                                     },
                                     running);
        ok &= expect(opened == QStringList({QStringLiteral("program-1/0"), QStringLiteral("program-1/1")}),
                     "groups: " + opened.join(' ').toStdString());
    }

    // Several relays report each Group once.
    {
        PublishConfig cfg;
        cfg.moqEndpoints = {QStringLiteral("mock://a"), QStringLiteral("mock://b")};
        cfg.moqEndpoint = cfg.moqEndpoints.first();
        cfg.fragmentDurationMs = 0;
        moq2ts::RedundantPublisher publisher;
        QStringList opened;
        std::mutex mutex;
        QObject::connect(&publisher, &moq2ts::RedundantPublisher::groupOpened, &publisher,
                         [&](const QString& track, qulonglong group) {
                             std::lock_guard lock(mutex);
                             opened << QStringLiteral("%1/%2").arg(track).arg(group);
                         },
                         Qt::DirectConnection);
        std::uint64_t group = 0;
        std::atomic<bool> running{true};
        ok &= expect(publisher.connect(cfg), "relays: connect");
        publisher.publishLiveObjects(cfg, QStringLiteral("program-1"), {}, QByteArray("{}"),
                                     [&]() -> std::optional<PublishedObject> {
                                         if (group == 3) {
                                             return std::nullopt;
                                         }
                                         PublishedObject object;
                                         object.trackName = QStringLiteral("program-1");
                                         object.groupId = group++;
                                         object.startsGroup = true;
                                         object.payload = QByteArray(188, 'x');
                                         return object;
                                     },
                                     running);
        ok &= expect(opened == QStringList({QStringLiteral("program-1/0"), QStringLiteral("program-1/1"),
                                            QStringLiteral("program-1/2")}),
                     "relays: " + opened.join(' ').toStdString());
    }

    return ok ? 0 : 1;
}
