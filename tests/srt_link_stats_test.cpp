#include "media/SrtSource.h"

#include <srt/srt.h>

#include <arpa/inet.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <iostream>
#include <string>
#include <thread>

using moq2ts::SrtSource;

namespace {
bool expect(bool cond, const std::string& msg) {
    if (!cond) { std::cerr << "FAIL: " << msg << '\n'; return false; }
    return true;
}
} // namespace

int main() {
    bool ok = true;

    // The figures of the [srt] line: plain names, RTT to 0.1 ms.
    SrtSource::LinkStats stats;
    stats.rttMs = 12.345;
    stats.received = 104857;
    stats.lost = 12;
    stats.retransmitted = 10;
    stats.dropped = 2;
    stats.bufferMs = 118;
    const std::string line = SrtSource::formatLinkStats(stats).toStdString();
    ok &= expect(line == "rtt_ms=12.3 recv=104857 lost=12 retrans=10 drop=2 buf_ms=118",
                 "the link figures read: " + line);

    // Nothing to print before the receive loop has read the socket once.
    SrtSource::LinkStatsSnapshot snapshot;
    ok &= expect(!snapshot.load().has_value(), "no figures before the first read");
    snapshot.store(stats);
    const std::optional<SrtSource::LinkStats> loaded = snapshot.load();
    ok &= expect(loaded.has_value() && loaded->received == 104857 && loaded->bufferMs == 118,
                 "the figures stored by the receive loop");

    // A source that connects and sends nothing still gets figures within
    // about a second, with nothing received: what an operator needs to see.
    {
        SrtSource::Config cfg;
        cfg.host = "127.0.0.1";
        cfg.port = 19874;
        cfg.mode = SrtSource::Config::Mode::Listener;
        int listenSock = -1;
        const QString err = SrtSource::listen(cfg, listenSock);
        int fds[2] = {-1, -1};
        if (expect(err.isEmpty(), "listen on 127.0.0.1:19874 failed (port busy?): " + err.toStdString()) &&
            expect(::pipe(fds) == 0, "pipe")) {
            std::atomic<bool> stop{false};
            SrtSource::LinkStatsSnapshot silent;
            std::thread loop([&] { SrtSource::listenLoop(listenSock, fds[1], stop, nullptr, nullptr, &silent); });
            const SRTSOCKET caller = srt_create_socket();
            sockaddr_in addr{};
            addr.sin_family = AF_INET;
            addr.sin_port = htons(cfg.port);
            ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
            ok &= expect(srt_connect(caller, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != SRT_ERROR,
                         "the silent caller connects");
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
            while (!silent.load().has_value() && std::chrono::steady_clock::now() < deadline) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
            const std::optional<SrtSource::LinkStats> idle = silent.load();
            ok &= expect(idle.has_value(), "figures while the source sends nothing");
            ok &= expect(idle.has_value() && idle->received == 0, "nothing received from a silent source");
            // The figures go when the connection does.
            srt_close(caller);
            const auto gone = std::chrono::steady_clock::now() + std::chrono::seconds(3);
            while (silent.load().has_value() && std::chrono::steady_clock::now() < gone) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
            ok &= expect(!silent.load().has_value(), "no figures once the caller has gone");
            stop.store(true, std::memory_order_release);
            loop.join();
            ::close(fds[0]);
            ::close(fds[1]);
            SrtSource::close(listenSock);
        }
        SrtSource::shutdown();
    }

    if (!ok) {
        return 1;
    }
    std::cout << "srt link stats tests passed\n";
    return 0;
}
