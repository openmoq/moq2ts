#include "publish/ReconnectPolicy.h"

#include <chrono>
#include <iostream>
#include <optional>
#include <string>

using moq2ts::ReconnectPolicy;
using std::chrono::seconds;

namespace {
bool expect(bool cond, const std::string& msg) {
    if (!cond) { std::cerr << "FAIL: " << msg << '\n'; return false; }
    return true;
}

std::string text(std::optional<seconds> delay) {
    return delay ? std::to_string(delay->count()) + " s" : "no retry";
}

// Before any connection worked: 2 s, 4 s, ... 18 s, then the publish ends at
// the 10th failed attempt.
bool firstConnectionIsBounded() {
    ReconnectPolicy policy;
    bool ok = true;
    for (int attempt = 1; attempt < ReconnectPolicy::kMaxFailedAttempts; ++attempt) {
        const std::optional<seconds> delay = policy.nextDelay(false);
        ok &= expect(delay == seconds(2 * attempt),
                     "attempt " + std::to_string(attempt) + " waits " + std::to_string(2 * attempt) + " s, not " + text(delay));
    }
    ok &= expect(!policy.nextDelay(false), "the 10th failed first connection ends the publish");
    return ok;
}

// After a working connection, including one that ended without a failure
// (a PAT change): 2 s more each time, up to 60 s, without a limit, and back
// to 2 s once a connection works again.
bool retriesWithoutLimitAfterAWorkingConnection() {
    ReconnectPolicy policy;
    policy.worked();
    bool ok = expect(policy.nextDelay(false) == seconds(2), "a lost working connection waits 2 s");
    for (int attempt = 2; attempt <= 100; ++attempt) {
        const std::optional<seconds> delay = policy.nextDelay(false);
        const seconds expected(std::min(2 * attempt, 60));
        ok &= expect(delay == expected, "attempt " + std::to_string(attempt) + " waits " +
                                            std::to_string(expected.count()) + " s, not " + text(delay));
    }
    policy.worked();
    ok &= expect(policy.nextDelay(false) == seconds(2), "a connection that worked again resets the wait to 2 s");
    ok &= expect(policy.nextDelay(false) == seconds(4), "and the schedule starts over");
    return ok;
}

// A permanent failure ends the publish at once before any connection worked.
// After a working connection, it follows the schedule like any other.
bool permanentOnlyAtStartup() {
    ReconnectPolicy first;
    bool ok = expect(!first.nextDelay(true), "a permanent failure before any working connection ends the publish");
    ReconnectPolicy later;
    later.worked();
    ok &= expect(later.nextDelay(true) == seconds(2), "a permanent failure after a working connection waits 2 s");
    ok &= expect(later.nextDelay(true) == seconds(4), "and keeps the schedule");
    return ok;
}
} // namespace

int main() {
    bool ok = true;
    ok &= firstConnectionIsBounded();
    ok &= retriesWithoutLimitAfterAWorkingConnection();
    ok &= permanentOnlyAtStartup();
    if (!ok) {
        return 1;
    }
    std::cout << "reconnect policy tests passed\n";
    return 0;
}
