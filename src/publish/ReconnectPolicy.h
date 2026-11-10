#pragma once

#include <algorithm>
#include <chrono>
#include <optional>

namespace moq2ts {

// When a live publish connects to the relay again after a failure. A
// permanent failure (moqxr's kFatal or kEndpointPermanent, such as a rejected
// publish) ends it at once. Otherwise it waits 2 s, then 2 s more after each
// failed attempt, up to 60 s. Until a connection has carried Objects, it tries
// at most kMaxFailedAttempts times, so that a configuration error exits
// non-zero: the waits add up to 90 s, plus the time each attempt takes to fail
// (up to about 16 s for a relay that never answers). After a working
// connection, a relay outage is transient, so the publish retries for as long
// as it lasts, as an SRT caller does.
class ReconnectPolicy {
public:
    static constexpr int kMaxFailedAttempts = 10;

    // The wait before the next attempt, or none when the publish ends.
    // established: the failed connection had carried Objects.
    std::optional<std::chrono::seconds> nextDelay(bool permanent, bool established) {
        if (permanent) {
            return std::nullopt;
        }
        if (established) {
            m_worked = true;
            m_failedAttempts = 0;
        }
        ++m_failedAttempts;
        if (!m_worked && m_failedAttempts >= kMaxFailedAttempts) {
            return std::nullopt;
        }
        return std::min(kDelayStep * m_failedAttempts, kMaxDelay);
    }

    // The failed attempts since the last connection that carried Objects.
    int failedAttempts() const { return m_failedAttempts; }

private:
    static constexpr std::chrono::seconds kDelayStep{2};
    static constexpr std::chrono::seconds kMaxDelay{60};
    bool m_worked = false;
    int m_failedAttempts = 0;
};

} // namespace moq2ts
