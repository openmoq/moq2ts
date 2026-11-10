#include "media/LivePipeline.h"

#include <QJsonArray>
#include <QJsonDocument>

#include <iostream>
#include <string>

using moq2ts::PublishedObject;
using moq2ts::TimelineHistory;
using moq2ts::timelineObject;

namespace {
bool expect(bool cond, const std::string& msg) {
    if (!cond) { std::cerr << "FAIL: " << msg << '\n'; return false; }
    return true;
}

PublishedObject mediaObject(std::uint64_t group) {
    PublishedObject media;
    media.trackName = QStringLiteral("program-1");
    media.groupId = group;
    media.objectId = 0;
    return media;
}

// The records of a timeline Object: [[mediaMs, [group, object], wallclockMs], ...].
QJsonArray recordsOf(const PublishedObject& timeline) {
    return QJsonDocument::fromJson(timeline.payload).array();
}
}  // namespace

int main() {
    bool ok = true;
    const QString track = QStringLiteral("program-1.timeline");
    constexpr std::uint64_t kGroupUs = 2000000;            // 2 s Groups
    constexpr std::uint64_t kStartWallUs = 1760000000000000ULL;
    // The steady clock when Group g is recorded: any origin, 2 s apart.
    const auto steadyAt = [](std::uint64_t g) { return static_cast<std::int64_t>(5'000'000 + g * kGroupUs); };

    // Without a window every record stays: the timeline grows with the broadcast.
    {
        TimelineHistory history;
        PublishedObject last;
        for (std::uint64_t g = 0; g < 300; ++g) {
            last = timelineObject(&history, track, mediaObject(g), g * kGroupUs, kStartWallUs + g * kGroupUs,
                                  steadyAt(g));
        }
        ok &= expect(recordsOf(last).size() == 300, "no window keeps every record");
    }

    // A window of 60 s keeps the records of the last 60 s: with 2 s Groups,
    // the newest and the 30 before it.
    {
        TimelineHistory history;
        PublishedObject last;
        for (std::uint64_t g = 0; g < 300; ++g) {
            last = timelineObject(&history, track, mediaObject(g), g * kGroupUs, kStartWallUs + g * kGroupUs,
                                  steadyAt(g), 60000);
            ok &= expect(recordsOf(last).size() <= 31, "the window bounds the records");
        }
        const QJsonArray records = recordsOf(last);
        ok &= expect(records.size() == 31, "a 60 s window of 2 s Groups holds 31 records");
        ok &= expect(records.last().toArray().at(1).toArray().at(0).toInt() == 299, "the newest record is the last Group");
        ok &= expect(records.first().toArray().at(1).toArray().at(0).toInt() == 269, "the oldest is 60 s behind it");
        ok &= expect(last.groupId == 299 && last.objectId == 0, "the Object is still Object 0 of the Group");
        ok &= expect(last.payload.size() < 31 * 60, "the payload stays small");
    }

    // The window follows the steady clock, so a wallclock step does not move
    // it. A step back of 1 h at Group 150 used to keep 1,951 records, and a
    // step forward every record but the newest.
    for (const std::int64_t stepUs : {-3'600'000'000LL, 3'600'000'000LL}) {
        TimelineHistory history;
        PublishedObject last;
        for (std::uint64_t g = 0; g < 300; ++g) {
            const std::int64_t wallUs = static_cast<std::int64_t>(kStartWallUs + g * kGroupUs) + (g >= 150 ? stepUs : 0);
            last = timelineObject(&history, track, mediaObject(g), g * kGroupUs, static_cast<std::uint64_t>(wallUs),
                                  steadyAt(g), 60000);
            if (g == 160) {
                ok &= expect(recordsOf(last).size() == 31,
                             "after a wallclock step of " + std::to_string(stepUs / 1000000) + " s, 31 records, not " +
                                 std::to_string(recordsOf(last).size()));
            }
        }
        ok &= expect(recordsOf(last).size() == 31, "a wallclock step leaves the window at 31 records");
    }

    // The newest record is always kept, even when it is wider than the window.
    {
        TimelineHistory history;
        timelineObject(&history, track, mediaObject(0), 0, kStartWallUs, steadyAt(0), 1);
        const PublishedObject second =
            timelineObject(&history, track, mediaObject(1), kGroupUs, kStartWallUs + kGroupUs, steadyAt(1), 1);
        ok &= expect(recordsOf(second).size() == 1, "a window shorter than a Group keeps the newest record");
    }

    return ok ? 0 : 1;
}
