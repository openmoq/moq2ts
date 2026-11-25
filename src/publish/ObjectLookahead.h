#pragma once

#include <QString>

#include <algorithm>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <optional>
#include <utility>
#include <vector>

#include "MoqxrPublisher.h"

namespace moq2ts {

// An Object ready to send, and whether it is the last of its Group on its
// track (final_in_subgroup).
struct ReadyObject {
    PublishedObject object;
    bool finalInGroup = false;
};

// Decides final_in_subgroup for a stream of Objects that interleaves several
// tracks. An Object is the last of its Group when the next Object of the SAME
// track starts another Group, so each track holds back its latest Object until
// its next one arrives. Holding one Object per track, rather than everything
// behind the oldest held Object, means a track that stalls or ends holds back
// its own last Object only, and every other track keeps flowing. MOQT orders
// nothing across tracks, so the cross-track order may change; the Objects of
// one track keep their order.
//
// An Object with finalInGroup set (a catalog or timeline Object) goes out at
// once, unless it describes a track that holds an Object (afterTrack): a
// timeline Object then goes out right after the media Object it records,
// which MSF -01 Section 7 requires to be published first. An endsTrack marker
// releases its track's held Object as final and is not sent. At the end of the
// source, the held Objects go out as final, in the order they arrived. An
// Object that ends the broadcast (endsBroadcast) is the source's last: the
// held Objects go out first, so it follows every Object it closes.
class ObjectLookahead {
public:
    explicit ObjectLookahead(std::function<std::optional<PublishedObject>()> source)
        : m_source(std::move(source)) {}

    // The next Object to send, or none at the end of the source. Reads from
    // the source only while nothing is ready. An idle Object comes back at
    // once, and the held Objects stay held.
    std::optional<ReadyObject> next() {
        while (m_ready.empty() && !m_ended) {
            std::optional<PublishedObject> object = m_source();
            if (!object.has_value()) {
                m_ended = true;
                releaseAll();
                break;
            }
            if (object->idle) {
                return ReadyObject{std::move(*object), false};
            }
            if (object->endsBroadcast) {
                releaseAll();
                m_ready.push_back({std::move(*object), true});
                continue;
            }
            const auto held = m_held.find(object->trackName);
            if (held != m_held.end()) {
                const bool final = object->endsTrack || object->groupId != held->second.object.groupId;
                release(std::move(held->second), final);
                m_held.erase(held);
            }
            if (object->endsTrack) {
                continue;
            }
            // An Object that describes a held Object waits for it.
            const auto described = object->afterTrack.isEmpty() ? m_held.end() : m_held.find(object->afterTrack);
            if (described != m_held.end()) {
                described->second.followers.push_back(std::move(*object));
            } else if (object->finalInGroup) {
                m_ready.push_back({std::move(*object), true});
            } else {
                const QString track = object->trackName;
                m_held[track] = Held{std::move(*object), m_arrivals++};
            }
        }
        if (m_ready.empty()) {
            return std::nullopt;
        }
        ReadyObject ready = std::move(m_ready.front());
        m_ready.pop_front();
        return ready;
    }

    // Objects read from the source and not yet returned.
    std::size_t pending() const { return m_held.size() + m_ready.size(); }

private:
    struct Held {
        PublishedObject object;
        std::uint64_t arrival = 0;
        // Objects that describe it (afterTrack), to send right after it:
        // one timeline Object at most, as one is made per media Group.
        std::vector<PublishedObject> followers;
    };

    void release(Held&& held, bool final) {
        m_ready.push_back({std::move(held.object), final});
        for (PublishedObject& follower : held.followers) {
            const bool followerFinal = follower.finalInGroup;
            m_ready.push_back({std::move(follower), followerFinal});
        }
    }

    void releaseAll() {
        std::vector<Held> held;
        for (auto& [track, entry] : m_held) {
            held.push_back(std::move(entry));
        }
        m_held.clear();
        std::sort(held.begin(), held.end(), [](const Held& a, const Held& b) { return a.arrival < b.arrival; });
        for (Held& entry : held) {
            release(std::move(entry), true);
        }
    }

    std::function<std::optional<PublishedObject>()> m_source;
    std::map<QString, Held> m_held;   // the latest Object of each track
    std::deque<ReadyObject> m_ready;
    std::uint64_t m_arrivals = 0;
    bool m_ended = false;
};

} // namespace moq2ts
