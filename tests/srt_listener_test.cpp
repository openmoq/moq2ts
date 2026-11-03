// SRT listener mode: an encoder that pushes the stream to us, one caller at a
// time. The second caller's data follows the first's in the pipe, and the
// source gap log records where the first caller's bytes end.
#include "media/SourceGapLog.h"
#include "media/SrtSource.h"

#include <srt/srt.h>

#include <QString>

#include <atomic>
#include <mutex>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include <syslog.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <unistd.h>

using moq2ts::SourceGapLog;
using moq2ts::SrtSource;
using Clock = std::chrono::steady_clock;

namespace {

constexpr std::size_t kPkt = 188;
constexpr int kPacketsPerCaller = 10;
constexpr uint16_t kPort = 19873;
// 5 TS packets per SRT payload. Some loopback setups silently drop UDP
// datagrams of 1.3 kB and more, which the usual 1316-byte payload would hit.
constexpr int kPayloadBytes = 5 * static_cast<int>(kPkt);

// What libsrt logs, at warning level and above.
std::mutex g_srtLogMutex;
std::vector<std::string> g_srtLog;

void captureSrtLog(void*, int, const char*, int, const char*, const char* message) {
    std::lock_guard<std::mutex> lock(g_srtLogMutex);
    g_srtLog.emplace_back(message);
}

int srtLogCount(const std::string& needle) {
    std::lock_guard<std::mutex> lock(g_srtLogMutex);
    int count = 0;
    for (const std::string& line : g_srtLog) {
        count += line.find(needle) != std::string::npos ? 1 : 0;
    }
    return count;
}

bool expect(bool cond, const std::string& msg) {
    if (!cond) {
        std::cerr << "FAIL: " << msg << '\n';
        return false;
    }
    return true;
}

// Every wait below is bounded; this catches anything that still blocks.
void onWatchdog(int) {
    static const char msg[] = "FAIL: srt listener test timed out (is port 19873 busy?)\n";
    [[maybe_unused]] const ssize_t n = ::write(STDERR_FILENO, msg, sizeof(msg) - 1);
    ::_exit(2);
}

// One TS packet whose byte 4 carries its sequence number.
std::vector<char> tsPacket(int seq) {
    std::vector<char> p(kPkt, static_cast<char>(0xAA));
    p[0] = 0x47;
    p[1] = 0x01;
    p[2] = 0x00;
    p[3] = 0x10;
    p[4] = static_cast<char>(seq);
    return p;
}

// Connects to the listener, sends count packets starting at firstSeq, and
// returns the still-open socket (SRT_INVALID_SOCK on failure).
SRTSOCKET callAndSend(int firstSeq, int count) {
    SRTSOCKET sock = srt_create_socket();
    if (sock == SRT_INVALID_SOCK) {
        return SRT_INVALID_SOCK;
    }
    int payload = kPayloadBytes;
    srt_setsockflag(sock, SRTO_PAYLOADSIZE, &payload, sizeof(payload));
    int timeoutMs = 3000;
    srt_setsockflag(sock, SRTO_CONNTIMEO, &timeoutMs, sizeof(timeoutMs));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(kPort);
    ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    if (srt_connect(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SRT_ERROR) {
        std::cerr << "caller: connect failed: " << srt_getlasterror_str() << '\n';
        srt_close(sock);
        return SRT_INVALID_SOCK;
    }
    std::vector<char> data;
    for (int i = 0; i < count; ++i) {
        const std::vector<char> p = tsPacket(firstSeq + i);
        data.insert(data.end(), p.begin(), p.end());
    }
    for (std::size_t off = 0; off < data.size();) {
        const int chunk = static_cast<int>(std::min<std::size_t>(kPayloadBytes, data.size() - off));
        if (srt_send(sock, data.data() + off, chunk) == SRT_ERROR) {
            std::cerr << "caller: send failed: " << srt_getlasterror_str() << '\n';
            srt_close(sock);
            return SRT_INVALID_SOCK;
        }
        off += static_cast<std::size_t>(chunk);
    }
    return sock;
}

// Reads exactly len bytes from the pipe within timeoutMs.
bool readExactly(int fd, std::vector<unsigned char>& out, std::size_t len, int timeoutMs) {
    const auto deadline = Clock::now() + std::chrono::milliseconds(timeoutMs);
    while (out.size() < len) {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
        if (left <= 0) {
            return false;
        }
        pollfd pfd{fd, POLLIN, 0};
        if (::poll(&pfd, 1, static_cast<int>(left)) <= 0) {
            return false;
        }
        unsigned char buf[4096];
        const ssize_t n = ::read(fd, buf, std::min(sizeof(buf), len - out.size()));
        if (n <= 0) {
            return false;
        }
        out.insert(out.end(), buf, buf + n);
    }
    return true;
}

} // namespace

int main() {
    std::signal(SIGALRM, onWatchdog);
    ::alarm(30);

    SrtSource::Config cfg;
    cfg.host = "127.0.0.1";
    cfg.port = kPort;
    cfg.mode = SrtSource::Config::Mode::Listener;
    int listenSock = -1;
    const QString err = SrtSource::listen(cfg, listenSock);
    if (!expect(err.isEmpty(), "listen on 127.0.0.1:19873 failed (port busy?): " + err.toStdString())) {
        return 1;
    }

    // Back to warnings, after listen() set libsrt's level: the check below
    // is on how the receive loop reads, not on what the level hides.
    srt_setloghandler(nullptr, captureSrtLog);
    srt_setloglevel(LOG_WARNING);

    int fds[2];
    if (!expect(::pipe(fds) == 0, "pipe")) {
        return 1;
    }
    std::atomic<bool> stop{false};
    SourceGapLog gaps;
    std::thread loop([&] { SrtSource::listenLoop(listenSock, fds[1], stop, nullptr, &gaps); });

    bool ok = true;
    std::vector<unsigned char> got;
    const std::size_t perCaller = kPacketsPerCaller * kPkt;

    SRTSOCKET first = callAndSend(0, kPacketsPerCaller);
    ok &= expect(first != SRT_INVALID_SOCK, "first caller could not connect and send");
    ok &= expect(readExactly(fds[0], got, perCaller, 5000), "first caller's packets did not reach the pipe");
    // A caller that stays connected and silent: the receive loop waits for
    // data, and reads none that is not there (libsrt warns "nothing to read"
    // on each such read, every receive timeout).
    std::this_thread::sleep_for(std::chrono::milliseconds(1200));
    const int emptyReads = srtLogCount("nothing to read");
    ok &= expect(emptyReads == 0, "a silent caller makes libsrt warn of " + std::to_string(emptyReads) +
                                      " reads with nothing to read");
    if (first != SRT_INVALID_SOCK) {
        srt_close(first);
    }
    ok &= expect(!gaps.next().has_value(), "no gap before a second caller connects");

    SRTSOCKET second = callAndSend(kPacketsPerCaller, kPacketsPerCaller);
    ok &= expect(second != SRT_INVALID_SOCK, "second caller could not connect and send");
    ok &= expect(readExactly(fds[0], got, 2 * perCaller, 5000), "second caller's packets did not reach the pipe");

    // The packets arrive whole and in order.
    if (got.size() == 2 * perCaller) {
        for (int i = 0; i < 2 * kPacketsPerCaller; ++i) {
            const unsigned char* p = got.data() + static_cast<std::size_t>(i) * kPkt;
            ok &= expect(p[0] == 0x47 && p[4] == static_cast<unsigned char>(i),
                         "packet " + std::to_string(i) + " out of order or damaged");
        }
    }

    // The gap sits where the second caller's bytes begin.
    const auto gapDeadline = Clock::now() + std::chrono::seconds(2);
    while (!gaps.next().has_value() && Clock::now() < gapDeadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    const auto gap = gaps.next();
    ok &= expect(gap.has_value() && *gap == perCaller,
                 "gap offset is " + (gap ? std::to_string(*gap) : std::string("absent")) + ", want " +
                     std::to_string(perCaller));
    // The gap names the new caller, not the receive buffer.
    unsigned causes = 0;
    const bool taken = gaps.takeUpTo(perCaller, &causes);
    ok &= expect(taken && causes == SourceGapLog::SrtNewCaller,
                 "gap cause is " + std::to_string(causes) + ", want SrtNewCaller");

    if (second != SRT_INVALID_SOCK) {
        srt_close(second);
    }
    const auto stopAt = Clock::now();
    stop.store(true, std::memory_order_release);
    loop.join();
    const auto stopMs = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - stopAt).count();
    ok &= expect(stopMs < 1000, "listenLoop took " + std::to_string(stopMs) + " ms to stop, want < 1000");

    ::close(fds[0]);
    ::close(fds[1]);
    SrtSource::close(listenSock);
    SrtSource::shutdown();

    if (ok) {
        std::cout << "srt listener tests passed\n";
        return 0;
    }
    return 1;
}
