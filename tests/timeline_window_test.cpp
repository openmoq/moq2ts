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

    // Without a window every record stays: the timeline grows with the broadcast.
    {
        TimelineHistory history;
        PublishedObject last;
        for (std::uint64_t g = 0; g < 300; ++g) {
            last = timelineObject(&history, track, mediaObject(g), g * kGroupUs, kStartWallUs + g * kGroupUs);
        }
        ok &= expect(recordsOf(last).size() == 300, "no window keeps every record");
    }

    // A window of 60 s keeps the records of the last 60 s of wallclock time:
    // with 2 s Groups, the newest and the 30 before it.
    {
        TimelineHistory history;
        PublishedObject last;
        for (std::uint64_t g = 0; g < 300; ++g) {
            last = timelineObject(&history, track, mediaObject(g), g * kGroupUs, kStartWallUs + g * kGroupUs, 60000);
            ok &= expect(recordsOf(last).size() <= 31, "the window bounds the records");
        }
        const QJsonArray records = recordsOf(last);
        ok &= expect(records.size() == 31, "a 60 s window of 2 s Groups holds 31 records");
        ok &= expect(records.last().toArray().at(1).toArray().at(0).toInt() == 299, "the newest record is the last Group");
        ok &= expect(records.first().toArray().at(1).toArray().at(0).toInt() == 269, "the oldest is 60 s behind it");
        ok &= expect(last.groupId == 299 && last.objectId == 0, "the Object is still Object 0 of the Group");
        ok &= expect(last.payload.size() < 31 * 60, "the payload stays small");
    }

    // A source without wallclock time (0) is trimmed on its media time.
    {
        TimelineHistory history;
        PublishedObject last;
        for (std::uint64_t g = 0; g < 100; ++g) {
            last = timelineObject(&history, track, mediaObject(g), g * kGroupUs, 0, 20000);
        }
        ok &= expect(recordsOf(last).size() == 11, "a 20 s window on media time holds 11 records");
    }

    // The newest record is always kept, even when it is wider than the window.
    {
        TimelineHistory history;
        timelineObject(&history, track, mediaObject(0), 0, kStartWallUs, 1);
        const PublishedObject second = timelineObject(&history, track, mediaObject(1), kGroupUs, kStartWallUs + kGroupUs, 1);
        ok &= expect(recordsOf(second).size() == 1, "a window shorter than a Group keeps the newest record");
    }

    return ok ? 0 : 1;
}
