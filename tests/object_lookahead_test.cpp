#include "publish/LiveSourceEnd.h"
#include "publish/ObjectLookahead.h"

#include <algorithm>
#include <deque>
#include <vector>
#include <iostream>
#include <string>

using moq2ts::ObjectLookahead;
using moq2ts::PublishedObject;
using moq2ts::ReadyObject;

namespace {
bool expect(bool cond, const std::string& msg) {
    if (!cond) { std::cerr << "FAIL: " << msg << '\n'; return false; }
    return true;
}

PublishedObject object(const char* track, std::uint64_t group, std::uint64_t id, bool finalInGroup = false) {
    PublishedObject o;
    o.trackName = QString::fromLatin1(track);
    o.groupId = group;
    o.objectId = id;
    o.finalInGroup = finalInGroup;
    o.payload = QByteArray(10, 'x');
    return o;
}

PublishedObject endOf(const char* track) {
    PublishedObject o;
    o.trackName = QString::fromLatin1(track);
    o.endsTrack = true;
    return o;
}

// The source has no Object yet.
PublishedObject nothingYet() {
    PublishedObject o;
    o.idle = true;
    return o;
}

// A source that hands out a script, then ends.
struct Script {
    std::deque<PublishedObject> objects;
    int reads = 0;
    std::function<std::optional<PublishedObject>()> source() {
        return [this]() -> std::optional<PublishedObject> {
            ++reads;
            if (objects.empty()) {
                return std::nullopt;
            }
            PublishedObject next = objects.front();
            objects.pop_front();
            return next;
        };
    }
};

std::string name(const ReadyObject& ready) {
    return ready.object.trackName.toStdString() + "/" + std::to_string(ready.object.groupId) + "/" +
           std::to_string(ready.object.objectId) + (ready.finalInGroup ? "F" : "");
}
}  // namespace

int main() {
    bool ok = true;

    // "Nothing yet" comes straight back, and releases no held Object: the
    // Group of a held one may still go on.
    {
        Script script;
        script.objects = {object("a", 0, 0), nothingYet(), object("a", 1, 0)};
        ObjectLookahead lookahead(script.source());
        const auto first = lookahead.next();
        ok &= expect(first && first->object.idle, "idle comes back at once");
        ok &= expect(lookahead.pending() == 1, "idle releases no held Object");
        std::string got;
        while (const auto next = lookahead.next()) {
            got += name(*next) + " ";
        }
        ok &= expect(got == "a/0/0F a/1/0F ", "after idle: " + got);
    }

    // What the publisher is given, and whether none means the end. "Nothing
    // yet" is none while not finished; the end of the source is none and
    // finished. Nothing yet after the closing Object is the end too: a second
    // read of the source must not look like a source that goes on.
    {
        using moq2ts::LiveSourceEnd;
        LiveSourceEnd end;
        ok &= expect(!end.take(ReadyObject{nothingYet(), false}).has_value() && !end.finished(),
                     "nothing yet: none, not finished");
        ok &= expect(end.take(ReadyObject{object("a", 0, 0), true}).has_value() && !end.finished(),
                     "an Object: published, not finished");
        ok &= expect(!end.take(std::nullopt).has_value() && end.finished(), "end of source: none, finished");
        LiveSourceEnd closing;
        PublishedObject last = object("catalog", 1, 0, true);
        last.endsBroadcast = true;
        ok &= expect(closing.take(ReadyObject{last, true}).has_value() && closing.finished(),
                     "the closing Object: published, then finished");
        ok &= expect(!closing.take(ReadyObject{nothingYet(), false}).has_value() && closing.finished(),
                     "nothing yet after the closing Object: still finished");
    }

    // One track: the last Object of each Group is final, and the end of the
    // source closes the last Group.
    {
        Script script;
        script.objects = {object("a", 0, 0), object("a", 0, 1), object("a", 1, 0), object("a", 1, 1)};
        ObjectLookahead lookahead(script.source());
        std::string got;
        while (const auto next = lookahead.next()) {
            got += name(*next) + " ";
        }
        ok &= expect(got == "a/0/0 a/0/1F a/1/0 a/1/1F ", "one track: " + got);
    }

    // Track A stops producing without ending while track B keeps producing:
    // B keeps flowing, and A holds back only its own last Object.
    {
        Script script;
        script.objects = {object("a", 0, 0), object("a", 0, 1)};
        for (int n = 0; n < 900; ++n) {
            script.objects.push_back(object("b", n / 3, n % 3));
        }
        ObjectLookahead lookahead(script.source());
        int returned = 0;
        int bReturned = 0;
        bool aLastReturned = false;
        bool bounded = true;
        for (int step = 0; step < 800; ++step) {
            const auto next = lookahead.next();
            if (!next) {
                break;
            }
            ++returned;
            bReturned += next->object.trackName == QLatin1String("b") ? 1 : 0;
            aLastReturned |= next->object.trackName == QLatin1String("a") && next->object.objectId == 1;
            // A's held Object, B's held Object, and what one read released.
            bounded &= lookahead.pending() <= 3 && script.reads <= returned + 3;
        }
        ok &= expect(bReturned == 799, "stalled track: B keeps flowing (" + std::to_string(bReturned) + ")");
        ok &= expect(!aLastReturned, "stalled track: A's last Object waits for A's next one");
        ok &= expect(bounded, "stalled track: nothing accumulates beyond one Object per track");
    }

    // An endsTrack marker makes A's last Object final at once, and is not sent.
    {
        Script script;
        script.objects = {object("a", 0, 0), object("a", 0, 1), endOf("a")};
        for (int n = 0; n < 10; ++n) {
            script.objects.push_back(object("b", 0, n));
        }
        ObjectLookahead lookahead(script.source());
        const auto first = lookahead.next();
        const auto second = lookahead.next();
        ok &= expect(first && name(*first) == "a/0/0", "end of track: A's first Object");
        ok &= expect(second && name(*second) == "a/0/1F", "end of track: A's last Object is final");
        ok &= expect(script.reads == 3, "end of track: released without reading further");
        int markers = 0;
        while (const auto next = lookahead.next()) {
            markers += next->object.endsTrack ? 1 : 0;
        }
        ok &= expect(markers == 0, "end of track: the marker is not sent");
    }

    // A finalInGroup Object goes out at once; the end of the source releases
    // the held Objects as final in arrival order.
    {
        Script script;
        script.objects = {object("b", 4, 0), object("a", 2, 0), object("catalog", 1, 0, true)};
        ObjectLookahead lookahead(script.source());
        std::string got;
        while (const auto next = lookahead.next()) {
            got += name(*next) + " ";
        }
        ok &= expect(got == "catalog/1/0F b/4/0F a/2/0F ", "final Objects and end of source: " + got);
    }

    // A timeline Object follows the media Object it records (MSF -01 Section
    // 7: data about previously published Groups). It waits with a held media
    // Object, is not lost when the media track stalls, and goes out right
    // after it when the media track's next Object, end, or the end of the
    // source releases it.
    {
        const auto timeline = [](std::uint64_t group) {
            PublishedObject o = object("a.timeline", group, 0, true);
            o.afterTrack = QStringLiteral("a");
            return o;
        };
        // Released by the next Object of the media track.
        Script next;
        next.objects = {object("a", 0, 0), timeline(0), object("a", 0, 1), object("a", 1, 0), timeline(1)};
        ObjectLookahead byNext(next.source());
        std::string got;
        while (const auto ready = byNext.next()) {
            got += name(*ready) + " ";
        }
        ok &= expect(got == "a/0/0 a.timeline/0/0F a/0/1F a/1/0F a.timeline/1/0F ",
                     "timeline after media, released by the next Object: " + got);

        // Stalled media track: B flows, the timeline Object waits with A's
        // held Object, and endsTrack releases both, media first.
        Script stalled;
        stalled.objects = {object("a", 0, 0), timeline(0)};
        for (int n = 0; n < 50; ++n) {
            stalled.objects.push_back(object("b", n, 0));
        }
        stalled.objects.push_back(endOf("a"));
        ObjectLookahead byEnd(stalled.source());
        std::vector<std::string> order;
        bool bounded = true;
        while (const auto ready = byEnd.next()) {
            order.push_back(name(*ready));
            bounded &= byEnd.pending() <= 4;
        }
        const auto at = [&order](const std::string& item) {
            return std::find(order.begin(), order.end(), item) - order.begin();
        };
        ok &= expect(at("a/0/0F") < static_cast<long>(order.size()) && at("a.timeline/0/0F") == at("a/0/0F") + 1,
                     "stalled media: the timeline Object goes out right after its media Object");
        ok &= expect(at("a/0/0F") > at("b/48/0F"), "stalled media: both wait for the end of A while B flows");
        ok &= expect(bounded, "stalled media: one timeline Object per held media Object at most");

        // The end of the source flushes the held media Object, then its
        // timeline Object.
        Script flushed;
        flushed.objects = {object("a", 0, 0), timeline(0)};
        ObjectLookahead byFlush(flushed.source());
        got.clear();
        while (const auto ready = byFlush.next()) {
            got += name(*ready) + " ";
        }
        ok &= expect(got == "a/0/0F a.timeline/0/0F ", "end of source: media, then timeline: " + got);
    }

    // The Object that ends the broadcast (MSF -01 Section 11.3) follows the
    // held last Object of every track, not the other way round.
    {
        Script script;
        PublishedObject closing = object("catalog", 2, 0, true);
        closing.endsBroadcast = true;
        script.objects = {object("a", 0, 0), object("b", 0, 0), closing};
        ObjectLookahead lookahead(script.source());
        std::string got;
        while (const auto next = lookahead.next()) {
            got += name(*next) + " ";
        }
        ok &= expect(got == "a/0/0F b/0/0F catalog/2/0F ", "closing catalog goes out last: " + got);
    }

    std::cerr << (ok ? "OK\n" : "FAILED\n");
    return ok ? 0 : 1;
}
