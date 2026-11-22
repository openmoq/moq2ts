#pragma once

#include <QByteArray>
#include <QString>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <optional>

#include "MoqxrPublisher.h"

namespace moq2ts {

// The bound of each relay path's queue, in payload bytes: about 2 s at 20 Mb/s.
inline constexpr std::int64_t kPathQueueBytes = 5 * 1024 * 1024;

// The Objects that one relay path of a RedundantPublisher has yet to send.
// The producer pushes every Object to the queue of each path and never waits:
// the QByteArray payloads are shared, not copied.
//
// A full queue drops whole Groups only. A Group that the path has started to
// send is always finished, even past the bound: moq2ts marks every subgroup as
// holding its Group's largest Object, so a cut Group would end with a FIN that
// tells the relay, and every subscriber it serves, that the Group is complete.
// A Group that has not started is dropped whole when it does not fit: its
// queued Objects are taken back out, with its timeline Object (afterTrack names
// the media track), and the rest of it is dropped as it comes. The bound can
// therefore be exceeded by the rest of one started Group per track, and by a
// Group that has no other Group of its track queued, which is taken whatever
// its size (a Group larger than the bound would else never go out). Catalog
// Objects and the endsTrack and endsBroadcast markers are never dropped: they
// are small, and a path needs them to send the latest catalog and to end its
// tracks. Thread-safe.
class PathQueue {
public:
    struct Counters {
        std::int64_t droppedObjects = 0;
        std::int64_t droppedBytes = 0;
        std::int64_t queuedObjects = 0;
        std::int64_t queuedBytes = 0;
        // Objects the path took, and whether it took the end of the queue or
        // the closing catalog.
        std::int64_t popped = 0;
        bool endTaken = false;
    };

    explicit PathQueue(std::int64_t maxBytes = kPathQueueBytes)
        : m_maxBytes(maxBytes) {}

    // Empties the queue and takes Objects again, for a new publish. The drop
    // counters add up across publishes.
    void reopen() {
        std::lock_guard lock(m_mutex);
        m_objects.clear();
        m_bytes = 0;
        m_dropping.clear();
        m_lastGroup.clear();
        m_sending.clear();
        m_closed = false;
        m_endTaken = false;
    }

    // Queues object, or drops it as the class comment says. Never blocks.
    void push(const PublishedObject& object) {
        {
            std::lock_guard lock(m_mutex);
            if (m_closed) {
                return;
            }
            const bool keep = admit(object);
            if (isMedia(object)) {
                m_lastGroup[object.trackName] = object.groupId;
            }
            if (!keep) {
                ++m_droppedObjects;
                m_droppedBytes += object.payload.size();
                return;
            }
            m_bytes += object.payload.size();
            m_objects.push_back(object);
        }
        m_ready.notify_one();
    }

    // The next Object, after waiting up to timeout for one. None when the wait
    // ran out, or when the queue is closed and empty.
    std::optional<PublishedObject> pop(std::chrono::milliseconds timeout) {
        std::unique_lock lock(m_mutex);
        m_ready.wait_for(lock, timeout, [this] { return !m_objects.empty() || m_closed; });
        if (m_objects.empty()) {
            m_endTaken = m_endTaken || m_closed;
            return std::nullopt;
        }
        PublishedObject object = std::move(m_objects.front());
        m_objects.pop_front();
        m_bytes -= object.payload.size();
        ++m_popped;
        if (isMedia(object)) {
            m_sending[object.trackName] = object.groupId;  // this Group is now being sent
        }
        m_endTaken = m_endTaken || object.endsBroadcast;
        return object;
    }

    // No more Objects come: pop() hands out what is left, then ends. discard
    // drops what is left, for a path that has ended.
    void close(bool discard = false) {
        {
            std::lock_guard lock(m_mutex);
            m_closed = true;
            if (discard) {
                for (const PublishedObject& object : m_objects) {
                    ++m_droppedObjects;
                    m_droppedBytes += object.payload.size();
                }
                m_objects.clear();
                m_bytes = 0;
            }
        }
        m_ready.notify_all();
    }

    bool drained() const {
        std::lock_guard lock(m_mutex);
        return m_closed && m_objects.empty();
    }

    // At the start of a new connection of the path: drops the queued media
    // and timeline Objects, which are stale after a lost connection, and the
    // rest of their Groups, so that the path goes on at the next Group that
    // starts. Returns the latest queued catalog, which the connection sends
    // first, or none. The markers stay.
    std::optional<QByteArray> flushForResume() {
        std::lock_guard lock(m_mutex);
        std::optional<QByteArray> catalog;
        std::deque<PublishedObject> kept;
        for (PublishedObject& object : m_objects) {
            if (object.endsTrack || object.endsBroadcast) {
                kept.push_back(std::move(object));
                continue;
            }
            m_bytes -= object.payload.size();
            if (object.trackName == QLatin1String(kCatalogTrackName)) {
                catalog = object.payload;
            } else {
                ++m_droppedObjects;
                m_droppedBytes += object.payload.size();
            }
        }
        m_objects.swap(kept);
        for (const auto& [track, group] : m_lastGroup) {
            m_dropping[track] = group;
        }
        m_sending.clear();  // the new connection starts no Group yet
        return catalog;
    }

    Counters counters() const {
        std::lock_guard lock(m_mutex);
        return Counters{
            .droppedObjects = m_droppedObjects,
            .droppedBytes = m_droppedBytes,
            .queuedObjects = static_cast<std::int64_t>(m_objects.size()),
            .queuedBytes = m_bytes,
            .popped = m_popped,
            .endTaken = m_endTaken,
        };
    }

private:
    static bool isMedia(const PublishedObject& object) {
        return !object.endsTrack && !object.endsBroadcast && object.afterTrack.isEmpty() &&
               object.trackName != QLatin1String(kCatalogTrackName);
    }

    // Whether object goes into the queue. Called with m_mutex held.
    bool admit(const PublishedObject& object) {
        if (object.endsTrack || object.endsBroadcast || object.trackName == QLatin1String(kCatalogTrackName)) {
            return true;
        }
        // A timeline Object goes with its media Group.
        if (!object.afterTrack.isEmpty()) {
            const auto described = m_dropping.find(object.afterTrack);
            return described == m_dropping.end() || described->second != object.groupId;
        }
        const auto dropping = m_dropping.find(object.trackName);
        if (dropping != m_dropping.end()) {
            if (dropping->second == object.groupId) {
                return false;
            }
            m_dropping.erase(dropping);  // a new Group
        }
        const auto sending = m_sending.find(object.trackName);
        if (sending != m_sending.end() && sending->second == object.groupId) {
            return true;  // a started Group is finished, past the bound if need be
        }
        if (m_bytes + object.payload.size() > m_maxBytes && !aloneOnTrack(object)) {
            takeBackGroup(object.trackName, object.groupId);
            m_dropping[object.trackName] = object.groupId;
            return false;
        }
        return true;
    }

    // Whether no Object of another Group of object's track is queued. A Group
    // that is alone on its track is taken even when it, or the other tracks
    // queued, take the queue past the bound: else a Group larger than the bound
    // would never be sent, and every Group of the path would be dropped.
    bool aloneOnTrack(const PublishedObject& object) const {
        for (const PublishedObject& queued : m_objects) {
            if (queued.groupId != object.groupId && !queued.endsTrack && !queued.endsBroadcast &&
                (queued.trackName == object.trackName || queued.afterTrack == object.trackName)) {
                return false;
            }
        }
        return true;
    }

    // Takes the queued Objects of a Group that has not started back out, with
    // its timeline Object, and counts them as dropped. Called with m_mutex held.
    void takeBackGroup(const QString& track, std::uint64_t group) {
        std::deque<PublishedObject> kept;
        for (PublishedObject& queued : m_objects) {
            const bool ofGroup = queued.groupId == group && !queued.endsTrack && !queued.endsBroadcast &&
                                 (queued.trackName == track || queued.afterTrack == track);
            if (ofGroup) {
                ++m_droppedObjects;
                m_droppedBytes += queued.payload.size();
                m_bytes -= queued.payload.size();
            } else {
                kept.push_back(std::move(queued));
            }
        }
        m_objects.swap(kept);
    }

    const std::int64_t m_maxBytes;
    mutable std::mutex m_mutex;
    std::condition_variable m_ready;
    std::deque<PublishedObject> m_objects;
    std::int64_t m_bytes = 0;
    // The Group of each track whose remaining Objects are dropped.
    std::map<QString, std::uint64_t> m_dropping;
    // The Group of the latest Object pushed on each media track.
    std::map<QString, std::uint64_t> m_lastGroup;
    // The Group each media track is sending: the Group of its last popped Object.
    std::map<QString, std::uint64_t> m_sending;
    bool m_closed = false;
    bool m_endTaken = false;
    std::int64_t m_droppedObjects = 0;
    std::int64_t m_droppedBytes = 0;
    std::int64_t m_popped = 0;
};

} // namespace moq2ts
