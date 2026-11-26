#include "publish/PathQueue.h"
#include "publish/RedundantPublisher.h"

#include <QStringList>

#include <atomic>
#include <chrono>
#include <deque>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using moq2ts::IMoqOutput;
using moq2ts::kCatalogTrackName;
using moq2ts::PathQueue;
using moq2ts::PublishConfig;
using moq2ts::PublishedObject;
using moq2ts::RedundantPublisher;

namespace {
bool expect(bool cond, const std::string& msg) {
    if (!cond) { std::cerr << "FAIL: " << msg << '\n'; return false; }
    return true;
}

PublishedObject media(std::uint64_t group, std::uint64_t id) {
    PublishedObject o;
    o.trackName = QStringLiteral("v");
    o.groupId = group;
    o.objectId = id;
    o.startsGroup = id == 0;
    o.payload = QByteArray(100, 'v');
    return o;
}

// The timeline Object of a media Group, as LivePipeline makes it.
PublishedObject timeline(std::uint64_t group) {
    PublishedObject o;
    o.trackName = QStringLiteral("v.timeline");
    o.groupId = group;
    o.finalInGroup = true;
    o.afterTrack = QStringLiteral("v");
    o.payload = QByteArray(20, 't');
    return o;
}

PublishedObject catalog(std::uint64_t group, bool endsBroadcast = false) {
    PublishedObject o;
    o.trackName = QLatin1String(kCatalogTrackName);
    o.groupId = group;
    o.startsGroup = true;
    o.finalInGroup = true;
    o.endsBroadcast = endsBroadcast;
    o.payload = QByteArray(30, static_cast<char>('0' + group));
    return o;
}

PublishedObject endOf(const char* track) {
    PublishedObject o;
    o.trackName = QString::fromLatin1(track);
    o.endsTrack = true;
    return o;
}

// One media Group of 4 Objects of 100 bytes, with its timeline Object of 20
// bytes after the first.
void appendGroup(std::deque<PublishedObject>& objects, std::uint64_t group) {
    objects.push_back(media(group, 0));
    objects.push_back(timeline(group));
    for (std::uint64_t id = 1; id < 4; ++id) {
        objects.push_back(media(group, id));
    }
}

std::string name(const PublishedObject& o) {
    if (o.endsTrack) {
        return o.trackName.toStdString() + "-end";
    }
    if (o.trackName == QLatin1String(kCatalogTrackName)) {
        return "c" + std::to_string(o.groupId) + (o.endsBroadcast ? "E" : "");
    }
    if (!o.afterTrack.isEmpty()) {
        return "t" + std::to_string(o.groupId);
    }
    return "v" + std::to_string(o.groupId) + "/" + std::to_string(o.objectId);
}

std::string names(const std::vector<PublishedObject>& objects) {
    std::string got;
    for (const PublishedObject& o : objects) {
        got += name(o) + " ";
    }
    return got;
}

std::string drain(PathQueue& queue, int most = 1 << 30) {
    std::string got;
    for (int n = 0; n < most; ++n) {
        const std::optional<PublishedObject> o = queue.pop(std::chrono::milliseconds(0));
        if (!o.has_value()) {
            break;
        }
        got += name(*o) + " ";
    }
    return got;
}

// A relay path that publishes whatever it is given. With fails set it fails
// at once; with a gate it takes nothing until the gate opens, as a stalled
// relay does.
class TestPath final : public IMoqOutput {
public:
    explicit TestPath(bool fails = false, const std::atomic<bool>* gate = nullptr)
        : m_fails(fails), m_gate(gate) {}

    bool connect(const PublishConfig& cfg) override {
        config = cfg;
        m_connected = true;
        return true;
    }
    bool publishLiveObjects(const PublishConfig& cfg, const QString&, const QStringList&, const QByteArray&,
                            std::function<std::optional<PublishedObject>()> nextObject,
                            std::atomic<bool>&) override {
        config = cfg;
        if (m_fails) {
            return false;
        }
        while (m_gate != nullptr && !m_gate->load() && m_connected) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        while (std::optional<PublishedObject> object = nextObject()) {
            if (object->idle) {
                idles.fetch_add(1);
                continue;
            }
            const bool hangs = (stallAfter >= 0 && static_cast<int>(objects.size()) + 1 == stallAfter) ||
                               (hangAtEnd && object->endsBroadcast);
            objects.push_back(std::move(*object));
            received.fetch_add(1);
            // A relay that goes quiet holds the publish until it is stopped.
            while (hangs && m_connected) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
        }
        // As a MoqxrPublisher that holds its session open before the close.
        for (const auto until = std::chrono::steady_clock::now() + holdAfterEnd;
             std::chrono::steady_clock::now() < until && m_connected;) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        stoppedBeforeReturn = !m_connected;
        return true;
    }
    void stop() override { m_connected = false; }
    bool connected() const override { return m_connected; }

    // Read once the publish has returned: its threads have ended then.
    PublishConfig config;
    std::vector<PublishedObject> objects;
    std::atomic<int> received{0};
    // How often its source said it had no Object yet.
    std::atomic<int> idles{0};
    // Goes quiet after taking this many Objects, or after the end of the
    // broadcast, until stop().
    int stallAfter = -1;
    bool hangAtEnd = false;
    // How long it takes to return once its source has ended.
    std::chrono::milliseconds holdAfterEnd{0};
    bool stoppedBeforeReturn = false;

private:
    bool m_fails = false;
    const std::atomic<bool>* m_gate = nullptr;
    std::atomic<bool> m_connected{false};
};

PublishConfig twoRelays() {
    PublishConfig cfg;
    cfg.moqEndpoints = {QStringLiteral("moqt://a.example:4433"), QStringLiteral("moqt://b.example:4433")};
    cfg.moqEndpoint = cfg.moqEndpoints.first();
    cfg.liveSource = true;
    return cfg;
}
}  // namespace

int main() {
    bool ok = true;

    // One relay keeps the MoqxrPublisher alone; several need the composite.
    {
        PublishConfig one;
        one.moqEndpoint = QStringLiteral("moqt://a.example:4433");
        ok &= expect(!RedundantPublisher::needed(one), "one endpoint: no RedundantPublisher");
        ok &= expect(RedundantPublisher::needed(twoRelays()), "two endpoints: RedundantPublisher");
        RedundantPublisher publisher([](int) { return std::make_unique<TestPath>(); }, moq2ts::kPathQueueBytes);
        ok &= expect(!publisher.connect(one), "the composite refuses one endpoint");
    }

    // While its queue is empty and open, a path's source says "nothing yet",
    // so that its publisher can serve the relay. The producer's own "nothing
    // yet" is not queued.
    {
        std::vector<TestPath*> paths;
        RedundantPublisher publisher(
            [&paths](int) {
                auto path = std::make_unique<TestPath>();
                paths.push_back(path.get());
                return path;
            },
            moq2ts::kPathQueueBytes);
        const PublishConfig cfg = twoRelays();
        ok &= expect(publisher.connect(cfg), "idle: two paths connect");
        std::atomic<bool> running{true};
        int step = 0;
        bool quietSeen = false;
        const auto source = [&]() -> std::optional<PublishedObject> {
            if (step++ == 0) {
                // Wait for both paths to report a quiet queue.
                for (int i = 0; i < 400 && !(paths[0]->idles > 0 && paths[1]->idles > 0); ++i) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                }
                quietSeen = paths[0]->idles > 0 && paths[1]->idles > 0;
                PublishedObject nothing;
                nothing.idle = true;
                return nothing;
            }
            if (step == 2) {
                PublishedObject media;
                media.trackName = QStringLiteral("program-1");
                media.payload = QByteArray(10, 'x');
                media.startsGroup = true;
                return media;
            }
            return std::nullopt;
        };
        ok &= expect(publisher.publishLiveObjects(cfg, QStringLiteral("program-1"), {}, QByteArray("{}"), source,
                                                  running),
                     "idle: publish succeeds");
        ok &= expect(quietSeen, "idle: a quiet queue reaches the path as idle");
        for (TestPath* path : paths) {
            ok &= expect(path->objects.size() == 1 && !path->objects.front().idle,
                         "idle: the producer's idle Object is not queued");
        }
    }

    // Both paths get every Object, with the same Group and Object IDs, in
    // order, and with the payload shared rather than copied. Each path gets
    // its own relay only.
    {
        std::vector<TestPath*> paths;
        RedundantPublisher publisher(
            [&paths](int) {
                auto path = std::make_unique<TestPath>();
                paths.push_back(path.get());
                return path;
            },
            moq2ts::kPathQueueBytes);
        const PublishConfig cfg = twoRelays();
        ok &= expect(publisher.connect(cfg), "two paths connect");
        std::deque<PublishedObject> script;
        for (std::uint64_t group = 0; group < 10; ++group) {
            appendGroup(script, group);
        }
        script.push_back(catalog(1, true));
        const std::vector<PublishedObject> sent(script.begin(), script.end());
        std::atomic<bool> running{true};
        const bool published = publisher.publishLiveObjects(
            cfg, QStringLiteral("v"), {QStringLiteral("v.timeline")}, QByteArray("{}"),
            [&script]() -> std::optional<PublishedObject> {
                if (script.empty()) {
                    return std::nullopt;
                }
                PublishedObject next = script.front();
                script.pop_front();
                return next;
            },
            running);
        ok &= expect(published, "the publish succeeds");
        ok &= expect(paths.size() == 2, "one path per relay");
        if (paths.size() == 2) {
            ok &= expect(paths[0]->config.moqEndpoint == cfg.moqEndpoints.at(0) &&
                             paths[1]->config.moqEndpoint == cfg.moqEndpoints.at(1),
                         "each path publishes to its own relay");
            ok &= expect(paths[0]->config.moqEndpoints.isEmpty() && paths[1]->config.moqEndpoints.isEmpty(),
                         "a path sees one relay");
            ok &= expect(names(paths[0]->objects) == names(sent), "path 0 gets everything: " + names(paths[0]->objects));
            ok &= expect(names(paths[1]->objects) == names(sent), "path 1 gets everything: " + names(paths[1]->objects));
            bool shared = paths[0]->objects.size() == sent.size() && paths[1]->objects.size() == sent.size();
            for (std::size_t n = 0; shared && n < sent.size(); ++n) {
                shared = paths[0]->objects[n].payload.constData() == sent[n].payload.constData() &&
                         paths[1]->objects[n].payload.constData() == sent[n].payload.constData();
            }
            ok &= expect(shared, "both paths share the payloads");
        }
    }

    // A stalled path: its queue (1000 bytes here) drops whole Groups that do
    // not fit, with their timeline Objects, and keeps the catalogs. It never
    // sends part of a Group. The other path gets everything, and the producer
    // never waits for the stalled one.
    {
        std::vector<TestPath*> paths;
        std::atomic<bool> sourceEnded{false};
        RedundantPublisher publisher(
            [&paths, &sourceEnded](int index) {
                auto path = std::make_unique<TestPath>(false, index == 1 ? &sourceEnded : nullptr);
                paths.push_back(path.get());
                return path;
            },
            1000);
        const PublishConfig cfg = twoRelays();
        ok &= expect(publisher.connect(cfg), "stalled: two paths connect");
        std::deque<PublishedObject> script;
        for (std::uint64_t group = 0; group < 5; ++group) {
            appendGroup(script, group);
            if (group == 2) {
                script.push_back(catalog(1));
            }
        }
        script.push_back(catalog(2, true));
        const std::vector<PublishedObject> sent(script.begin(), script.end());
        int handed = 0;
        std::atomic<bool> running{true};
        const bool published = publisher.publishLiveObjects(
            cfg, QStringLiteral("v"), {QStringLiteral("v.timeline")}, QByteArray("{}"),
            [&]() -> std::optional<PublishedObject> {
                // Path 0 keeps up: the next Object comes once it has the last.
                const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
                while (paths[0]->received.load() < handed && std::chrono::steady_clock::now() < deadline) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
                if (script.empty()) {
                    sourceEnded.store(true);
                    return std::nullopt;
                }
                PublishedObject next = script.front();
                script.pop_front();
                ++handed;
                return next;
            },
            running);
        ok &= expect(published, "stalled: the publish succeeds");
        if (paths.size() == 2) {
            ok &= expect(names(paths[0]->objects) == names(sent), "stalled: path 0 gets everything: " +
                                                                      names(paths[0]->objects));
            const std::string expected = "v0/0 t0 v0/1 v0/2 v0/3 v1/0 t1 v1/1 v1/2 v1/3 c1 c2E ";
            ok &= expect(names(paths[1]->objects) == expected, "stalled: path 1 gets " + names(paths[1]->objects));
            ok &= expect(publisher.pathFinalFields(0).contains(QStringLiteral("dropped_objects=0 ")),
                         "stalled: path 0 drops nothing: " + publisher.pathFinalFields(0).toStdString());
            ok &= expect(publisher.pathFinalFields(1).contains(QStringLiteral("dropped_objects=15 ")),
                         "stalled: path 1 counts 15 drops: " + publisher.pathFinalFields(1).toStdString());
        }
    }

    // A queue that has room again takes the next Group from its start.
    {
        PathQueue fast(1000);
        PathQueue slow(1000);
        std::string fastGot;
        const auto push = [&](const PublishedObject& o) {
            fast.push(o);
            slow.push(o);
            fastGot += drain(fast);
        };
        std::deque<PublishedObject> stream;
        for (std::uint64_t group = 0; group < 5; ++group) {
            appendGroup(stream, group);
            if (group == 2) {
                stream.push_back(catalog(1));
            }
        }
        for (const PublishedObject& o : stream) {
            push(o);
        }
        ok &= expect(slow.counters().queuedBytes <= 1000, "queue: the bound holds");
        ok &= expect(drain(slow, 5) == "v0/0 t0 v0/1 v0/2 v0/3 ", "queue: Group 0 goes out");
        std::deque<PublishedObject> more;
        appendGroup(more, 5);
        more.push_back(catalog(2, true));
        for (const PublishedObject& o : more) {
            push(o);
        }
        const std::string slowGot = drain(slow);
        ok &= expect(slowGot == "v1/0 t1 v1/1 v1/2 v1/3 c1 v5/0 t5 v5/1 v5/2 v5/3 c2E ",
                     "queue: the slow path resumes at Group 5: " + slowGot);
        ok &= expect(fastGot.find("v3/0 t3 v3/1") != std::string::npos && fastGot.find("v4/3 v5/0") != std::string::npos,
                     "queue: the fast path gets everything: " + fastGot);
        ok &= expect(fast.counters().droppedObjects == 0 && slow.counters().droppedObjects == 15,
                     "queue: only the slow path drops");
    }

    // A Group the path has started is finished even past the bound, and the
    // next Group that does not fit is dropped whole: no Group is ever cut.
    {
        PathQueue queue(250);
        std::deque<PublishedObject> stream;
        appendGroup(stream, 0);
        appendGroup(stream, 1);
        queue.push(stream.front());  // v0/0
        stream.pop_front();
        const std::string first = drain(queue, 1);
        ok &= expect(first == "v0/0 ", "started: the path starts Group 0: " + first);
        for (const PublishedObject& o : stream) {
            queue.push(o);
        }
        ok &= expect(queue.counters().queuedBytes > 250, "started: the bound gives way to finish Group 0");
        const std::string rest = drain(queue);
        ok &= expect(rest == "t0 v0/1 v0/2 v0/3 ", "started: Group 0 is finished, Group 1 is dropped: " + rest);
        ok &= expect(queue.counters().droppedObjects == 5, "started: Group 1 counts 5 drops");
    }

    // A new connection drops the queued media and timeline Objects and the
    // rest of their Groups, hands back the latest catalog, and keeps the
    // markers. The path goes on at the next Group.
    {
        PathQueue queue(1000);
        const PublishedObject newer = catalog(1);
        for (const PublishedObject& o : {media(0, 0), timeline(0), media(0, 1), newer, media(1, 0), media(1, 1),
                                         endOf("w")}) {
            queue.push(o);
        }
        const std::optional<QByteArray> flushed = queue.flushForResume();
        ok &= expect(flushed.has_value() && *flushed == newer.payload, "flush: the latest catalog comes back");
        ok &= expect(drain(queue) == "w-end ", "flush: only the marker stays");
        for (const PublishedObject& o : {media(1, 2), timeline(1), media(2, 0), timeline(2), media(2, 1)}) {
            queue.push(o);
        }
        const std::string got = drain(queue);
        ok &= expect(got == "v2/0 t2 v2/1 ", "flush: the path goes on at Group 2: " + got);
    }

    // One path that fails does not fail the publish; every path failing does.
    for (const bool bothFail : {false, true}) {
        std::vector<TestPath*> paths;
        RedundantPublisher publisher(
            [&paths, bothFail](int index) {
                auto path = std::make_unique<TestPath>(index == 1 || bothFail);
                paths.push_back(path.get());
                return path;
            },
            moq2ts::kPathQueueBytes);
        int errors = 0;
        QObject::connect(&publisher, &RedundantPublisher::publishError, [&errors](const QString&) { ++errors; });
        const PublishConfig cfg = twoRelays();
        ok &= expect(publisher.connect(cfg), "failing: two paths connect");
        std::deque<PublishedObject> script;
        for (std::uint64_t group = 0; group < 3; ++group) {
            appendGroup(script, group);
        }
        std::atomic<bool> running{true};
        const bool published = publisher.publishLiveObjects(
            cfg, QStringLiteral("v"), {QStringLiteral("v.timeline")}, QByteArray("{}"),
            [&script]() -> std::optional<PublishedObject> {
                if (script.empty()) {
                    return std::nullopt;
                }
                PublishedObject next = script.front();
                script.pop_front();
                return next;
            },
            running);
        if (bothFail) {
            ok &= expect(!published && errors == 1, "every path failing fails the publish");
        } else {
            ok &= expect(published && errors == 0, "one path failing does not fail the publish");
            ok &= expect(paths.size() == 2 && paths[0]->objects.size() == 15, "the other path gets everything");
        }
    }

    // A Group larger than the bound is still sent whole, and the Group after
    // it, while it is queued, is dropped whole.
    {
        PathQueue queue(150);
        std::deque<PublishedObject> stream;
        appendGroup(stream, 0);
        appendGroup(stream, 1);
        std::size_t n = 0;
        for (const PublishedObject& o : stream) {
            queue.push(o);
            if (++n == 5) {
                break;  // Group 0, 420 bytes, with nothing else queued
            }
        }
        std::deque<PublishedObject> next;
        appendGroup(next, 1);
        for (const PublishedObject& o : next) {
            queue.push(o);
        }
        const std::string got = drain(queue);
        ok &= expect(got == "v0/0 t0 v0/1 v0/2 v0/3 ", "oversized: Group 0 is sent whole: " + got);
        ok &= expect(queue.counters().droppedObjects == 5, "oversized: Group 1 is dropped whole");
        for (const PublishedObject& o : {media(2, 0), timeline(2), media(2, 1)}) {
            queue.push(o);
        }
        ok &= expect(drain(queue) == "v2/0 t2 v2/1 ", "oversized: Group 2 goes on once Group 0 has gone");
    }

    // After a flush, a timeline Object of a dropped media Group goes, and one
    // of the next Group stays even when it comes before its media Object.
    {
        PathQueue queue(1000);
        for (const PublishedObject& o : {media(0, 0), timeline(0), media(0, 1)}) {
            queue.push(o);
        }
        queue.flushForResume();
        for (const PublishedObject& o : {timeline(0), timeline(1), media(1, 0), media(1, 1)}) {
            queue.push(o);
        }
        const std::string got = drain(queue);
        ok &= expect(got == "t1 v1/0 v1/1 ", "flush: timeline Objects follow their media Groups: " + got);
    }

    // The end of the publish waits for a slow path, not for one that has
    // stopped. A path that took nothing is given the idle time, one that
    // stalled after starting the drain time, one that took the end of the
    // queue and hangs the idle time again.
    struct Case {
        const char* label;
        int stallAfter;
        bool hangAtEnd;
        bool gated;
        std::chrono::milliseconds least;
        std::chrono::milliseconds most;
    };
    for (const Case& c : {Case{"idle", -1, false, true, std::chrono::milliseconds(380), std::chrono::milliseconds(3000)},
                          Case{"stalled", 4, false, false, std::chrono::milliseconds(100), std::chrono::milliseconds(300)},
                          Case{"at end", -1, true, false, std::chrono::milliseconds(380), std::chrono::milliseconds(3000)}}) {
        std::vector<TestPath*> paths;
        const std::atomic<bool> never{false};
        RedundantPublisher publisher(
            [&](int index) {
                auto path = std::make_unique<TestPath>(false, index == 1 && c.gated ? &never : nullptr);
                if (index == 1) {
                    path->stallAfter = c.stallAfter;
                    path->hangAtEnd = c.hangAtEnd;
                }
                paths.push_back(path.get());
                return path;
            },
            moq2ts::kPathQueueBytes);
        publisher.setTimeouts(std::chrono::milliseconds(120), std::chrono::milliseconds(400));
        PublishConfig cfg = twoRelays();
        cfg.closeDelayMs = 0;
        ok &= expect(publisher.connect(cfg), std::string("timeout ") + c.label + ": connects");
        std::deque<PublishedObject> script;
        appendGroup(script, 0);
        appendGroup(script, 1);
        script.push_back(catalog(1, true));
        std::atomic<bool> running{true};
        const auto began = std::chrono::steady_clock::now();
        const bool published = publisher.publishLiveObjects(
            cfg, QStringLiteral("v"), {QStringLiteral("v.timeline")}, QByteArray("{}"),
            [&script]() -> std::optional<PublishedObject> {
                if (script.empty()) {
                    return std::nullopt;
                }
                PublishedObject next = script.front();
                script.pop_front();
                return next;
            },
            running);
        const auto took = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - began);
        ok &= expect(published, std::string("timeout ") + c.label + ": the other path publishes");
        ok &= expect(took >= c.least && took <= c.most,
                     std::string("timeout ") + c.label + ": the hung path is stopped after its limit, not before or never (" +
                         std::to_string(took.count()) + " ms)");
    }

    // A path that has taken the end holds its session for the close delay
    // before it finishes: that time does not count as hanging.
    {
        std::vector<TestPath*> paths;
        RedundantPublisher publisher(
            [&paths](int) {
                auto path = std::make_unique<TestPath>();
                path->holdAfterEnd = std::chrono::milliseconds(700);
                paths.push_back(path.get());
                return path;
            },
            moq2ts::kPathQueueBytes);
        publisher.setTimeouts(std::chrono::milliseconds(120), std::chrono::milliseconds(400));
        PublishConfig cfg = twoRelays();
        cfg.closeDelayMs = 600;
        ok &= expect(publisher.connect(cfg), "close delay: connects");
        std::deque<PublishedObject> script;
        appendGroup(script, 0);
        script.push_back(catalog(1, true));
        std::atomic<bool> running{true};
        publisher.publishLiveObjects(
            cfg, QStringLiteral("v"), {QStringLiteral("v.timeline")}, QByteArray("{}"),
            [&script]() -> std::optional<PublishedObject> {
                if (script.empty()) {
                    return std::nullopt;
                }
                PublishedObject next = script.front();
                script.pop_front();
                return next;
            },
            running);
        for (const TestPath* path : paths) {
            ok &= expect(!path->stoppedBeforeReturn, "close delay: a path is not stopped while it holds its session");
        }
    }

    // The relays of a publisher stay those of its first connect().
    {
        RedundantPublisher publisher([](int) { return std::make_unique<TestPath>(); }, moq2ts::kPathQueueBytes);
        int errors = 0;
        QObject::connect(&publisher, &RedundantPublisher::publishError, [&errors](const QString&) { ++errors; });
        PublishConfig cfg = twoRelays();
        ok &= expect(publisher.connect(cfg), "endpoints: first connect");
        ok &= expect(publisher.connect(cfg), "endpoints: the same relays connect again");
        cfg.moqEndpoints = {QStringLiteral("moqt://a.example:4433"), QStringLiteral("moqt://c.example:4433")};
        ok &= expect(!publisher.connect(cfg) && errors == 1, "endpoints: other relays are refused");
        ok &= expect(publisher.statsFields() == QStringLiteral("sent_objects=unknown sent_bytes=unknown"),
                     "stats: no sent count is unknown: " + publisher.statsFields().toStdString());
    }

    if (ok) {
        std::cout << "redundant publisher tests passed\n";
    }
    return ok ? 0 : 1;
}
