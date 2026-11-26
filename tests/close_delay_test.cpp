// When a publish holds the session open before it closes it, and for how long.
#include "publish/CloseDelay.h"
#include "publish/LiveSourceEnd.h"

#include <chrono>
#include <iostream>
#include <optional>

namespace {

int failures = 0;

void check(bool condition, const char* what) {
    if (!condition) {
        std::cerr << "FAIL: " << what << '\n';
        ++failures;
    }
}

moq2ts::ReadyObject object(bool endsBroadcast) {
    moq2ts::ReadyObject ready;
    ready.object.endsBroadcast = endsBroadcast;
    return ready;
}

} // namespace

int main() {
    using moq2ts::holdsBeforeClose;
    // holdsBeforeClose(published, sourceEnded, liveSource, endedBroadcast)
    check(holdsBeforeClose(true, true, true, true), "a live source that ended its broadcast holds");
    check(holdsBeforeClose(true, true, false, false), "a file published to its end holds");
    check(!holdsBeforeClose(true, true, true, false), "a live session without a closing catalog (PAT change) closes at once");
    check(!holdsBeforeClose(true, false, true, false), "a session that ended before its source closes at once");
    check(!holdsBeforeClose(true, false, false, false), "a file publish that gave up closes at once");
    check(!holdsBeforeClose(false, true, true, true), "a failed publish closes at once");

    {
        moq2ts::LiveSourceEnd end;
        end.take(object(false));
        end.take(std::nullopt);
        check(end.finished() && !end.endedBroadcast(), "a source that only ran out did not end a broadcast");
    }
    {
        moq2ts::LiveSourceEnd end;
        end.take(object(true));
        check(end.endedBroadcast(), "the closing Object ends the broadcast");
    }

    using namespace std::chrono_literals;
    const auto timed = [](std::chrono::milliseconds delay, auto keepWaiting) {
        const auto started = std::chrono::steady_clock::now();
        moq2ts::waitWhile(delay, keepWaiting);
        return std::chrono::steady_clock::now() - started;
    };
    check(timed(200ms, [] { return true; }) >= 200ms, "the whole delay passes while nothing stops it");
    {
        int calls = 0;
        const auto elapsed = timed(10000ms, [&calls] { return ++calls < 3; });
        check(elapsed < 2000ms, "a stop ends the wait early");
    }
    {
        int calls = 0;
        const auto elapsed = timed(0ms, [&calls] { ++calls; return true; });
        check(elapsed < 100ms && calls == 0, "no delay, no wait");
    }
    return failures == 0 ? 0 : 1;
}
