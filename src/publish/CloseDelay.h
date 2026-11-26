#pragma once

#include <algorithm>
#include <chrono>
#include <thread>

namespace moq2ts {

// moqxr closes once its data is acknowledged, which does not mean that the
// relay application has read it: a relay may drop what it has not read when
// the connection closes, and the end of the stream is then lost. A publish
// that delivered the whole of its source therefore holds the session open for
// a while first. A live source says that it did with the catalog that ends the
// broadcast; a session without one (a PAT change, an error) goes on to the
// next step at once.
inline bool holdsBeforeClose(bool published, bool sourceEnded, bool liveSource, bool endedBroadcast) {
    return published && sourceEnded && (!liveSource || endedBroadcast);
}

// Waits for delay, or until keepWaiting() returns false.
template <typename Predicate>
void waitWhile(std::chrono::milliseconds delay, Predicate keepWaiting) {
    const auto until = std::chrono::steady_clock::now() + delay;
    for (auto now = std::chrono::steady_clock::now(); now < until && keepWaiting();
         now = std::chrono::steady_clock::now()) {
        std::this_thread::sleep_for(
            std::min<std::chrono::steady_clock::duration>(until - now, std::chrono::milliseconds(20)));
    }
}

} // namespace moq2ts
