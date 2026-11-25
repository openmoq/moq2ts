#pragma once

#include <atomic>
#include <optional>
#include <utility>

#include "ObjectLookahead.h"

namespace moq2ts {

// A live publisher asks its source for the next Object and gets none both when
// there is nothing yet and when the source has ended; finished() tells the two
// apart. take() is called from the source's thread, finished() from any.
class LiveSourceEnd {
public:
    // What to publish of what the source gave: the Object, or none.
    std::optional<ReadyObject> take(std::optional<ReadyObject> ready) {
        if (!ready.has_value()) {
            m_finished.store(true, std::memory_order_release);
            return std::nullopt;
        }
        if (ready->object.idle) {
            return std::nullopt;
        }
        // The closing Object is the source's last, so a "nothing yet" that
        // follows it must not read as a source that goes on.
        if (ready->object.endsBroadcast) {
            m_finished.store(true, std::memory_order_release);
        }
        return ready;
    }

    bool finished() const { return m_finished.load(std::memory_order_acquire); }

private:
    std::atomic<bool> m_finished{false};
};

} // namespace moq2ts
