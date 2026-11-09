#include "publish/ReconnectBudget.h"

#include <iostream>
#include <string>

using moq2ts::ReconnectBudget;

namespace {
bool expect(bool cond, const std::string& msg) {
    if (!cond) { std::cerr << "FAIL: " << msg << '\n'; return false; }
    return true;
}

// A relay that never answers: the publish gives up after the bound, which
// also covers a first connection that never succeeds.
bool givesUpAfterTheBound() {
    ReconnectBudget budget;
    bool ok = true;
    for (int attempt = 1; attempt < ReconnectBudget::kMaxFailedAttempts; ++attempt) {
        ok &= expect(budget.allowRetry(false, false), "attempt " + std::to_string(attempt) + " may retry");
    }
    ok &= expect(!budget.allowRetry(false, false), "the last attempt within the bound ends the publish");
    return ok;
}

// A permanent failure ends the publish at once, on the first connection or
// after a working one.
bool stopsOnAPermanentFailure() {
    bool ok = true;
    ReconnectBudget first;
    ok &= expect(!first.allowRetry(true, false), "a permanent failure of the first connection ends the publish");
    ReconnectBudget later;
    ok &= expect(!later.allowRetry(true, true), "a permanent failure after a working connection ends the publish");
    return ok;
}

// A connection that carried Objects restarts the count, so a relay that
// restarts now and then never exhausts the budget.
bool aWorkingConnectionRestartsTheCount() {
    ReconnectBudget budget;
    bool ok = true;
    for (int round = 0; round < 3; ++round) {
        for (int attempt = 1; attempt < ReconnectBudget::kMaxFailedAttempts; ++attempt) {
            ok &= expect(budget.allowRetry(false, false), "failed attempt before the bound may retry");
        }
        ok &= expect(budget.allowRetry(false, true), "a lost working connection may retry");
    }
    return ok;
}
} // namespace

int main() {
    bool ok = true;
    ok &= givesUpAfterTheBound();
    ok &= stopsOnAPermanentFailure();
    ok &= aWorkingConnectionRestartsTheCount();
    if (!ok) {
        return 1;
    }
    std::cout << "reconnect budget tests passed\n";
    return 0;
}
