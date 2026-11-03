#include "SrtSource.h"

#include "SourceGapLog.h"
#include "TsRingBuffer.h"

#include <srt/srt.h>

#include <QDebug>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <limits.h>
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <syslog.h>
#include <unistd.h>

namespace moq2ts {

namespace {
bool& srtInitialized() {
    static bool initialized = false;
    return initialized;
}

void ensureSrtInit() {
    if (!srtInitialized()) {
        srt_startup();
        // libsrt logs warnings (LOG_NOTICE by default) on stderr, from its own
        // threads. Errors only: moq2ts reports what matters from the return
        // codes and statistics.
        srt_setloglevel(LOG_ERR);
        srtInitialized() = true;
    }
}

// Socket options shared by both modes. They must be set before connect/listen;
// a listener's accepted sockets inherit them.
void applyOptions(SRTSOCKET sock, const SrtSource::Config& cfg) {
    int latency = cfg.latencyMs;
    srt_setsockflag(sock, SRTO_RCVLATENCY, &latency, sizeof(latency));
    srt_setsockflag(sock, SRTO_PEERLATENCY, &latency, sizeof(latency));
    int timeout = cfg.timeoutMs;
    srt_setsockflag(sock, SRTO_RCVTIMEO, &timeout, sizeof(timeout));
    // Receive headroom. Absorbs transient scheduling delays so the kernel/SRT
    // buffers do not overflow and force packet drops.
    if (cfg.rcvBufBytes > 0) {
        int rcvBuf = cfg.rcvBufBytes;
        srt_setsockflag(sock, SRTO_RCVBUF, &rcvBuf, sizeof(rcvBuf));
    }
    if (cfg.udpRcvBufBytes > 0) {
        int udpRcvBuf = cfg.udpRcvBufBytes;
        srt_setsockflag(sock, SRTO_UDP_RCVBUF, &udpRcvBuf, sizeof(udpRcvBuf));
    }
    int no = 0;
    srt_setsockflag(sock, SRTO_SNDSYN, &no, sizeof(no)); // non-blocking send (unused but safe)
}
} // namespace

QString SrtSource::connect(const Config& cfg, int& sockOut) {
    ensureSrtInit();
    sockOut = SRT_INVALID_SOCK;

    struct addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    const std::string portStr = std::to_string(cfg.port);
    if (getaddrinfo(cfg.host.c_str(), portStr.c_str(), &hints, &res) != 0 || !res) {
        return QStringLiteral("SRT: cannot resolve %1:%2").arg(QString::fromStdString(cfg.host)).arg(cfg.port);
    }

    SRTSOCKET sock = srt_create_socket();
    if (sock == SRT_INVALID_SOCK) {
        freeaddrinfo(res);
        return QStringLiteral("SRT: srt_create_socket failed: %1").arg(srt_getlasterror_str());
    }

    applyOptions(sock, cfg);

    // Connect (caller mode)
    if (srt_connect(sock, res->ai_addr, static_cast<int>(res->ai_addrlen)) == SRT_ERROR) {
        const QString err = QStringLiteral("SRT: connect to %1:%2 failed: %3")
                                .arg(QString::fromStdString(cfg.host))
                                .arg(cfg.port)
                                .arg(srt_getlasterror_str());
        srt_close(sock);
        freeaddrinfo(res);
        return err;
    }
    freeaddrinfo(res);

    sockOut = sock;
    return QString();
}

QString SrtSource::listen(const Config& cfg, int& listenSockOut) {
    ensureSrtInit();
    listenSockOut = SRT_INVALID_SOCK;

    struct addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_flags = AI_PASSIVE;
    const std::string portStr = std::to_string(cfg.port);
    if (getaddrinfo(cfg.host.c_str(), portStr.c_str(), &hints, &res) != 0 || !res) {
        return QStringLiteral("SRT: cannot resolve %1:%2").arg(QString::fromStdString(cfg.host)).arg(cfg.port);
    }

    SRTSOCKET sock = srt_create_socket();
    if (sock == SRT_INVALID_SOCK) {
        freeaddrinfo(res);
        return QStringLiteral("SRT: srt_create_socket failed: %1").arg(srt_getlasterror_str());
    }
    applyOptions(sock, cfg);

    if (srt_bind(sock, res->ai_addr, static_cast<int>(res->ai_addrlen)) == SRT_ERROR ||
        srt_listen(sock, 1) == SRT_ERROR) {
        const QString err = QStringLiteral("SRT: listen on %1:%2 failed: %3")
                                .arg(QString::fromStdString(cfg.host))
                                .arg(cfg.port)
                                .arg(srt_getlasterror_str());
        srt_close(sock);
        freeaddrinfo(res);
        return err;
    }
    freeaddrinfo(res);

    listenSockOut = sock;
    return QString();
}

void SrtSource::receiveLoop(int sock, int pipeFd, std::atomic<bool>& stopRequested,
                            const std::atomic<int>* resyncPid, SourceGapLog* gaps,
                            std::uint64_t* outputOffset) {
    // SRT delivers TS in pkt_size chunks (typically 1316 = 7x188).
    //
    // The receive path must never block. Writing straight to the pipe did block:
    // while the publisher waits for its first subscriber nothing drains the pipe,
    // so once it filled, ::write() stalled, srt_recv() stopped being called, SRT's
    // receive buffer overflowed and the sender began discarding packets - the feed
    // degraded within seconds.
    //
    // Instead, received data goes into a bounded ring buffer and the pipe is
    // written non-blocking. While the reader is stalled the ring keeps the most
    // recent media and discards the oldest, so the wait is effectively unbounded;
    // when the reader starts draining, output tries to resume at a random
    // access point near the live edge rather than replaying stale media. The
    // ring's safety fallback can resume without one.
    constexpr int kBufSize = 1316 * 4;                            // up to 28 TS packets per recv
    constexpr std::size_t kRingCapacityBytes = 4u * 1024 * 1024;  // ~5 s at 6 Mbit/s

    // Non-blocking pipe: a stalled reader must never stall the receiver.
    const int flags = ::fcntl(pipeFd, F_GETFL, 0);
    if (flags >= 0) {
        ::fcntl(pipeFd, F_SETFL, flags | O_NONBLOCK);
    }
#ifdef F_SETPIPE_SZ
    // The ring drains into the pipe only when srt_recv() returns, and libsrt
    // releases packets in bursts, so each burst can move at most what the pipe
    // holds. At the 64 KiB default that barely exceeds a 45 Mbit/s feed: after
    // any delay the reader cannot catch up, the ring stays full, and it keeps
    // overflowing. A larger pipe lets one burst carry a backlog. Best effort:
    // the default stays when the system limit is lower.
    constexpr int kPipeBytes = 1024 * 1024;
    if (::fcntl(pipeFd, F_SETPIPE_SZ, kPipeBytes) < 0) {
        qWarning() << "[SRT] Pipe kept at its default size:" << strerror(errno);
    }
#endif

    TsRingBuffer ring(kRingCapacityBytes);
    std::uint64_t reportedDrops = 0;
    auto lastDropLog = std::chrono::steady_clock::now() - std::chrono::hours(1);
    auto lastStatsLog = lastDropLog;
    int64_t lastRcvDrop = 0;
    int64_t lastRcvLoss = 0;
    char buf[kBufSize];
    // Bytes written to the pipe so far: the offset of the next byte the reader
    // gets, which is where a gap falls in what it reads.
    std::uint64_t localWritten = 0;
    std::uint64_t& written = outputOffset != nullptr ? *outputOffset : localWritten;
    // A gap has been recorded and its resync has not completed yet.
    bool gapOpen = false;

    // The pipe only ever receives whole TS packets, in writes of at most
    // PIPE_BUF bytes. POSIX makes such a write to a non-blocking pipe all or
    // nothing, so a full pipe cannot accept part of a packet. If it could, a
    // drop could then discard the rest of that packet, and the reader would
    // lose the packet framing after the gap.
    constexpr std::size_t kTsPacketSize = TsRingBuffer::kTsPacketSize;
    constexpr std::size_t kMaxWrite = (PIPE_BUF / kTsPacketSize) * kTsPacketSize;
    static_assert(kMaxWrite >= kTsPacketSize, "PIPE_BUF holds at least one TS packet");

    // Push as much as the reader will currently take; never blocks.
    const auto drainToPipe = [&]() {
        if (ring.size() < kTsPacketSize) {
            return;
        }
        // Only touch the pipe - and only pay for a resync scan - when the reader
        // can actually accept data; otherwise leave everything buffered.
        struct pollfd pfd{};
        pfd.fd = pipeFd;
        pfd.events = POLLOUT;
        if (::poll(&pfd, 1, 0) <= 0 || (pfd.revents & POLLOUT) == 0) {
            return;
        }
        if (!ring.resyncToRandomAccess(resyncPid ? resyncPid->load(std::memory_order_relaxed) : -1)) {
            return;  // gap not yet re-anchored on an IDR; nothing safe to emit
        }
        gapOpen = false;
        char wrapped[kTsPacketSize];
        while (ring.size() >= kTsPacketSize) {
            const char* data = ring.frontData();
            std::size_t span = std::min(ring.frontContiguous(), kMaxWrite);
            span -= span % kTsPacketSize;
            if (span == 0) {
                // The next packet wraps around the end of the ring: write it
                // from a copy, still as one whole packet.
                for (std::size_t i = 0; i < kTsPacketSize; ++i) {
                    wrapped[i] = static_cast<char>(ring.at(i));
                }
                data = wrapped;
                span = kTsPacketSize;
            }
            const ssize_t n = ::write(pipeFd, data, span);
            if (n > 0) {
                ring.consume(static_cast<std::size_t>(n));
                written += static_cast<std::uint64_t>(n);
                continue;
            }
            if (n == 0) {
                return;  // nothing accepted; keep the rest buffered (never fatal)
            }
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return;  // reader is behind; keep the rest buffered
            }
            qWarning() << "[SRT] Pipe write failed:" << strerror(errno);
            stopRequested.store(true, std::memory_order_release);
            return;
        }
    };

    // Read only when the socket has data (or an error) to report. A blocking
    // srt_recv() that hits SRTO_RCVTIMEO on a silent peer makes libsrt 1.5
    // read its empty buffer and warn "nothing to read" on every timeout. The
    // wait is bounded so the stop flag and the pipe are checked as before.
    constexpr int kRecvPollMs = 200;
    const int eid = srt_epoll_create();
    if (eid < 0) {
        qWarning() << "[SRT] epoll_create failed:" << srt_getlasterror_str();
        return;
    }
    struct EpollRelease {
        int eid;
        ~EpollRelease() { srt_epoll_release(eid); }
    } epollRelease{eid};
    const int events = SRT_EPOLL_IN | SRT_EPOLL_ERR;
    if (srt_epoll_add_usock(eid, sock, &events) == SRT_ERROR) {
        qWarning() << "[SRT] Cannot wait for data on the socket:" << srt_getlasterror_str();
        return;
    }

    while (!stopRequested.load(std::memory_order_acquire)) {
        drainToPipe();

        SRTSOCKET ready[1];
        int readyCount = 1;
        if (srt_epoll_wait(eid, ready, &readyCount, nullptr, nullptr, kRecvPollMs, nullptr, nullptr, nullptr,
                           nullptr) == SRT_ERROR) {
            if (srt_getlasterror(nullptr) == SRT_ETIMEOUT) {
                continue;  // no data yet: check the stop flag and the pipe
            }
            qWarning() << "[SRT] Waiting for data failed:" << srt_getlasterror_str();
            break;
        }

        int received = srt_recv(sock, buf, kBufSize);
        if (received == SRT_ERROR) {
            int err = srt_getlasterror(nullptr);
            if (err == SRT_EASYNCRCV || err == SRT_ETIMEOUT) {
                continue; // timeout - check stop flag and retry
            }
            SRT_SOCKSTATUS st = srt_getsockstate(sock);
            if (st == SRTS_BROKEN || st == SRTS_CLOSING || st == SRTS_CLOSED) {
                qWarning() << "[SRT] Connection lost:" << srt_getlasterror_str();
                break;
            }
            continue;
        }
        if (received <= 0) {
            continue;
        }

        ring.append(buf, static_cast<std::size_t>(received));
        // The first drop of an episode fixes where the gap falls: everything
        // already written comes before it, and nothing more is written until
        // the ring resumes at a random access point.
        if (ring.resyncPending() && !gapOpen) {
            gapOpen = true;
            if (gaps != nullptr) {
                gaps->record(written);
            }
        }

        // Report ANY ring drop, rate-limited by time rather than volume. Reporting
        // only per buffer's-worth hid small drops entirely, which made a real media
        // gap look like it had no local cause.
        const std::uint64_t dropped = ring.droppedBytes();
        const auto now = std::chrono::steady_clock::now();
        if (dropped > reportedDrops && now - lastDropLog >= std::chrono::seconds(2)) {
            qWarning() << "[SRT] ring dropped" << (dropped - reportedDrops)
                       << "bytes (total" << dropped << ") - consumer behind; the published stream has a gap "
                          "and will try to resume at a random access point";
            reportedDrops = dropped;
            lastDropLog = now;
        }

        // SRT's own receive-side loss counters. These are the only way to see
        // packets SRT discarded because they missed the latency budget
        // (TLPKTDROP); such loss is invisible to every check downstream of here.
        if (now - lastStatsLog >= std::chrono::seconds(2)) {
            lastStatsLog = now;
            SRT_TRACEBSTATS st{};
            if (srt_bstats(sock, &st, 0) == 0) {
                if (st.pktRcvDropTotal != lastRcvDrop || st.pktRcvLossTotal != lastRcvLoss) {
                    qWarning() << "[SRT] receiver loss: dropped(TLPKTDROP)="
                               << st.pktRcvDropTotal << "(+" << (st.pktRcvDropTotal - lastRcvDrop)
                               << ") lost=" << st.pktRcvLossTotal
                               << "(+" << (st.pktRcvLossTotal - lastRcvLoss)
                               << ") retrans=" << st.pktRetransTotal;
                    lastRcvDrop = st.pktRcvDropTotal;
                    lastRcvLoss = st.pktRcvLossTotal;
                }
            }
        }
    }
}

void SrtSource::listenLoop(int listenSock, int pipeFd, std::atomic<bool>& stopRequested,
                           const std::atomic<int>* resyncPid, SourceGapLog* gaps) {
    // srt_accept() cannot be given a timeout, so wait for a pending caller on an
    // epoll with one and keep checking stopRequested between waits.
    constexpr int kAcceptPollMs = 200;
    const int eid = srt_epoll_create();
    if (eid < 0) {
        qWarning() << "[SRT] epoll_create failed:" << srt_getlasterror_str();
        return;
    }
    const int events = SRT_EPOLL_IN | SRT_EPOLL_ERR;
    srt_epoll_add_usock(eid, listenSock, &events);

    // Bytes written to the pipe across every caller: where the next caller's
    // data begins in what the reader gets.
    std::uint64_t written = 0;
    bool first = true;
    while (!stopRequested.load(std::memory_order_acquire)) {
        SRTSOCKET ready[2];
        int readyCount = 2;
        if (srt_epoll_wait(eid, ready, &readyCount, nullptr, nullptr, kAcceptPollMs, nullptr, nullptr,
                           nullptr, nullptr) == SRT_ERROR) {
            if (srt_getlasterror(nullptr) != SRT_ETIMEOUT) {
                qWarning() << "[SRT] Waiting for a caller failed:" << srt_getlasterror_str();
                break;
            }
            continue;
        }

        sockaddr_storage peer{};
        int peerLen = sizeof(peer);
        const SRTSOCKET sock = srt_accept(listenSock, reinterpret_cast<sockaddr*>(&peer), &peerLen);
        if (sock == SRT_INVALID_SOCK) {
            continue;  // the caller gave up before the accept
        }
        qInfo() << "[SRT] Caller connected";

        // A caller after the first one starts a new stretch of the stream: its
        // first byte is the next one the reader gets. Streams that bridge two
        // encoders can jump in time and PCR here.
        if (!first && gaps != nullptr) {
            gaps->record(written, SourceGapLog::SrtNewCaller);
        }
        first = false;

        receiveLoop(sock, pipeFd, stopRequested, resyncPid, gaps, &written);
        srt_close(sock);
        if (!stopRequested.load(std::memory_order_acquire)) {
            qInfo() << "[SRT] Caller disconnected; waiting for the next one";
        }
    }
    srt_epoll_release(eid);
}

void SrtSource::close(int sock) {
    if (sock != SRT_INVALID_SOCK) {
        srt_close(sock);
    }
}

void SrtSource::shutdown() {
    // srt_startup() must be paired with srt_cleanup() before the process exits.
    // Without it, libsrt's own static destructors run at exit (via __cxa_finalize)
    // while its internals are still live and jump through a stale function
    // pointer - a SIGSEGV during teardown, after all useful work is done.
    // Call this once, after every SRT socket is closed and the receive thread has
    // been joined. Safe to call when SRT was never started.
    if (srtInitialized()) {
        srt_cleanup();
        srtInitialized() = false;
    }
}

} // namespace moq2ts
