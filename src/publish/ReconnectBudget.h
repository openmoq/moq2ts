#pragma once

namespace moq2ts {

// Decides whether a live publish connects to the relay again after a failure.
// A permanent failure (a rejected publish, a protocol error) ends it at once.
// Otherwise it tries again, at most kMaxFailedAttempts times in a row without
// a connection that carries Objects. With the 1 s to 30 s backoff, the waits
// add up to about 2.5 minutes. The publish then fails, so that the process exits non-zero
// and a supervisor can see why.
class ReconnectBudget {
public:
    static constexpr int kMaxFailedAttempts = 10;

    // established: the failed connection had carried Objects.
    bool allowRetry(bool permanent, bool established) {
        if (permanent) {
            return false;
        }
        m_failedAttempts = established ? 0 : m_failedAttempts + 1;
        return m_failedAttempts < kMaxFailedAttempts;
    }

private:
    int m_failedAttempts = 0;
};

} // namespace moq2ts
