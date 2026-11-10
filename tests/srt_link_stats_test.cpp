#include "media/SrtSource.h"

#include <iostream>
#include <string>

using moq2ts::SrtSource;

namespace {
bool expect(bool cond, const std::string& msg) {
    if (!cond) { std::cerr << "FAIL: " << msg << '\n'; return false; }
    return true;
}
} // namespace

int main() {
    bool ok = true;

    // The figures of the [stats] line: plain names, RTT to 0.1 ms.
    SrtSource::LinkStats stats;
    stats.rttMs = 12.345;
    stats.received = 104857;
    stats.lost = 12;
    stats.retransmitted = 10;
    stats.dropped = 2;
    stats.bufferMs = 118;
    const std::string line = SrtSource::formatLinkStats(stats).toStdString();
    ok &= expect(line == "srt_rtt_ms=12.3 srt_recv=104857 srt_lost=12 srt_retrans=10 srt_drop=2 srt_buf_ms=118",
                 "the link figures read: " + line);

    // Nothing to print before the receive loop has read the socket once.
    SrtSource::LinkStatsSnapshot snapshot;
    ok &= expect(!snapshot.load().has_value(), "no figures before the first read");
    snapshot.store(stats);
    const std::optional<SrtSource::LinkStats> loaded = snapshot.load();
    ok &= expect(loaded.has_value() && loaded->received == 104857 && loaded->bufferMs == 118,
                 "the figures stored by the receive loop");

    if (!ok) {
        return 1;
    }
    std::cout << "srt link stats tests passed\n";
    return 0;
}
