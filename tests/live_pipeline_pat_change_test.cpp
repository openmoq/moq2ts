#include "media/LivePipeline.h"
#include "publish/MoqxrPublisher.h"
#include "ts_test_builder.h"

#include <QJsonArray>
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

QByteArray pcrPacket(int pid, std::int64_t pcr, int cc) {
    const std::int64_t base = pcr / 300;
    const int extension = static_cast<int>(pcr % 300);
    QByteArray packet;
    packet.append(char(0x47));
    packet.append(static_cast<char>((pid >> 8) & 0x1F));
    packet.append(static_cast<char>(pid & 0xFF));
    packet.append(static_cast<char>(0x30 | (cc & 0x0F)));
    packet.append(char(7));
    packet.append(char(0x10));
    packet.append(static_cast<char>((base >> 25) & 0xFF));
    packet.append(static_cast<char>((base >> 17) & 0xFF));
    packet.append(static_cast<char>((base >> 9) & 0xFF));
    packet.append(static_cast<char>((base >> 1) & 0xFF));
    packet.append(static_cast<char>(((base & 0x01) << 7) | 0x7E | ((extension >> 8) & 0x01)));
    packet.append(static_cast<char>(extension & 0xFF));
    packet.append(QByteArray(188 - packet.size(), char(0)));
    return packet;
}

struct Program {
    int number;
    int pmtPid;
    int videoPid;
};

// Rounds of 0.1 s of PCR time. Each repeats the PAT, which lists programs, and
// the PMT, the PCR, a random access PES packet and eight more video packets of
// every program. firstRound continues the clock across calls.
QByteArray rounds(const std::vector<Program>& programs, int firstRound, int count) {
    static int patCc = 0;
    static int pmtCc = 0;
    static int videoCc = 0;
    QList<std::pair<int, int>> pat;
    for (const Program& program : programs) {
        pat.append({program.number, program.pmtPid});
    }
    QByteArray ts;
    for (int round = firstRound; round < firstRound + count; ++round) {
        ts += tb::psiPacket(0, tb::patSection(pat), patCc++ & 0x0F);
        for (const Program& program : programs) {
            ts += tb::psiPacket(program.pmtPid, tb::pmtSection(program.number, program.videoPid, {{0x1B, program.videoPid}}),
                                pmtCc++ & 0x0F);
        }
        for (const Program& program : programs) {
            ts += pcrPacket(program.videoPid, static_cast<std::int64_t>(round) * 2700000, videoCc++ & 0x0F);
            ts += tb::tsPacket(program.videoPid, true, tb::pesHeaderWithPts(static_cast<std::uint64_t>(round) * 9000),
                               videoCc++ & 0x0F, 0x40);
            for (int k = 0; k < 8; ++k) {
                ts += tb::tsPacket(program.videoPid, false, QByteArray(184, char(k)), videoCc++ & 0x0F);
            }
        }
    }
    return ts;
}

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

// One publishLiveObjects call: the media track, the catalog it started with,
// and the Objects of the media track.
struct Session {
    QString track;
    QByteArray catalog;
    int mediaObjects = 0;
};

class TestPublisher final : public IMoqOutput {
public:
    bool connect(const PublishConfig&) override {
        m_connected = true;
        return true;
    }
    bool connected() const override { return m_connected; }
    void stop() override { m_connected = false; }

    bool publishLiveObjects(const PublishConfig&, const QString& mediaTrack, const QStringList&, const QByteArray& catalog,
                            std::function<std::optional<PublishedObject>()> nextObject,
                            std::atomic<bool>& running) override {
        Session session;
        session.track = mediaTrack;
        session.catalog = catalog;
        while (running.load(std::memory_order_acquire)) {
            std::optional<PublishedObject> object = nextObject();
            if (!object.has_value()) {
                break;
            }
            if (object->trackName == mediaTrack) {
                ++session.mediaObjects;
            }
        }
        std::lock_guard<std::mutex> lock(m_mutex);
        m_sessions.push_back(std::move(session));
        return true;
    }

    std::vector<Session> sessions() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_sessions;
    }

private:
    bool m_connected = false;
    mutable std::mutex m_mutex;
    std::vector<Session> m_sessions;
};

QJsonObject mediaTrack(const QByteArray& catalog, const QString& name) {
    for (const QJsonValue& track : QJsonDocument::fromJson(catalog).object().value(QStringLiteral("tracks")).toArray()) {
        if (track.toObject().value(QStringLiteral("name")).toString() == name) {
            return track.toObject();
        }
    }
    return {};
}

int timelineTrackCount(const QByteArray& catalog) {
    int count = 0;
    for (const QJsonValue& track : QJsonDocument::fromJson(catalog).object().value(QStringLiteral("tracks")).toArray()) {
        count += track.toObject().value(QStringLiteral("packaging")).toString() == QLatin1String("mediatimeline") ? 1 : 0;
    }
    return count;
}

int trackCount(const QByteArray& catalog) {
    int count = 0;
    for (const QJsonValue& track : QJsonDocument::fromJson(catalog).object().value(QStringLiteral("tracks")).toArray()) {
        count += track.toObject().value(QStringLiteral("packaging")).toString() == QLatin1String("mpeg2ts") ? 1 : 0;
    }
    return count;
}

// Runs an unmodified live publish of data through a FIFO.
std::vector<Session> publish(const QString& fifo, const QByteArray& data,
                             const std::function<void(PublishConfig&)>& adjust = {}) {
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
    cfg.transparentMode = true;
    cfg.fragmentDurationMs = 0;
    cfg.targetSegmentBytes = 4 * 188;
    if (adjust) {
        adjust(cfg);
    }
    publisher.connect(cfg);
    moq2ts::LivePipeline pipeline;
    pipeline.start(cfg, &publisher);
    const auto deadline = std::chrono::steady_clock::now() + kWait;
    while (pipeline.running() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    pipeline.stop();
    writer.join();
    return publisher.sessions();
}
}  // namespace

int main() {
    std::signal(SIGPIPE, SIG_IGN);
    bool ok = true;
    QTemporaryDir dir;
    const Program one{1, 0x1000, 0x100};
    const Program two{2, 0x1100, 0x200};

    // The single program changes its number: a new session with a new
    // unmodified-program track for program 2, and the old track is not in it.
    {
        const QByteArray data = rounds({one}, 0, 40) + rounds({two}, 40, 40);
        const std::vector<Session> sessions = publish(dir.filePath("renumber.fifo"), data);
        if (expect(sessions.size() == 2, "renumber: two sessions")) {
            ok &= expect(sessions[0].track == "program-1", "renumber: first track program-1");
            ok &= expect(mediaTrack(sessions[0].catalog, "program-1").value("mpeg2tsProgramNumber").toInt() == 1,
                         "renumber: first track is program 1");
            ok &= expect(sessions[0].mediaObjects > 0, "renumber: first session publishes");
            ok &= expect(sessions[1].track == "program-2", "renumber: second track program-2");
            const QJsonObject track = mediaTrack(sessions[1].catalog, "program-2");
            ok &= expect(track.value("mpeg2tsMode").toString() == "unmodified-program", "renumber: unmodified-program");
            ok &= expect(track.value("mpeg2tsProgramNumber").toInt() == 2, "renumber: new track is program 2");
            ok &= expect(trackCount(sessions[1].catalog) == 1 && mediaTrack(sessions[1].catalog, "program-1").isEmpty(),
                         "renumber: the old track is gone from the new catalog");
            ok &= expect(sessions[1].mediaObjects > 0, "renumber: second session publishes");
        }
    }

    // The PAT lists a second program: the new track is an unmodified multiplex.
    {
        const QByteArray data = rounds({one}, 0, 40) + rounds({one, two}, 40, 40);
        const std::vector<Session> sessions = publish(dir.filePath("second.fifo"), data);
        if (expect(sessions.size() == 2, "second program: two sessions")) {
            ok &= expect(sessions[1].track.startsWith("mux-2"), "second program: multiplex track mux-2");
            const QJsonObject track = mediaTrack(sessions[1].catalog, sessions[1].track);
            ok &= expect(track.value("mpeg2tsMode").toString() == "unmodified-multiplex", "second program: unmodified-multiplex");
            ok &= expect(trackCount(sessions[1].catalog) == 1, "second program: one mpeg2ts track");
            ok &= expect(sessions[1].mediaObjects > 0, "second program: second session publishes");
        }
    }

    // No PAT change: one session, as before.
    {
        const std::vector<Session> sessions = publish(dir.filePath("steady.fifo"), rounds({one}, 0, 60));
        ok &= expect(sessions.size() == 1 && sessions[0].track == "program-1", "steady: one session, program-1");
    }

    // The media timeline: the catalog declares the accessible window, which
    // the timeline is trimmed to; the window and the track can be turned off.
    {
        const QByteArray data = rounds({one}, 0, 40);
        const std::vector<Session> byDefault = publish(dir.filePath("timeline-default.fifo"), data);
        if (expect(byDefault.size() == 1, "timeline: one session")) {
            ok &= expect(mediaTrack(byDefault[0].catalog, "program-1").value("accessibleWindowTime").toInt() == 300000,
                         "timeline: the default window is 300 s");
            ok &= expect(timelineTrackCount(byDefault[0].catalog) == 1, "timeline: the track is published");
        }
        const std::vector<Session> unbounded = publish(dir.filePath("timeline-unbounded.fifo"), data,
                                                       [](PublishConfig& cfg) { cfg.accessibleWindowMs = 0; });
        if (expect(unbounded.size() == 1, "unbounded: one session")) {
            ok &= expect(!mediaTrack(unbounded[0].catalog, "program-1").contains("accessibleWindowTime"),
                         "unbounded: no window declared");
        }
        const std::vector<Session> none = publish(dir.filePath("timeline-none.fifo"), data,
                                                  [](PublishConfig& cfg) { cfg.timelineTrack = false; });
        if (expect(none.size() == 1, "no timeline: one session")) {
            ok &= expect(timelineTrackCount(none[0].catalog) == 0, "no timeline: no mediatimeline track");
            ok &= expect(none[0].mediaObjects > 0, "no timeline: the media still publishes");
        }
    }

    return ok ? 0 : 1;
}
