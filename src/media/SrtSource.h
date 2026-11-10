#pragma once

#include <QByteArray>
#include <QFile>
#include <QString>
#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>

namespace moq2ts {

class SourceGapLog;

/// Connects to an SRT source in caller mode (or listens for one in listener
/// mode) and delivers received TS packets to a callback. The callback receives raw TS data (188-byte packets) in the
/// same format as a file read - the existing M2tsPacketizer/LivePipeline can
/// consume it transparently via a pipe or QFile on /dev/stdin.
///
/// For moq2ts's m2ts packaging the SRT data is raw TS - no CMAF remux needed.
class SrtSource {
public:
    struct Config {
        // Caller: connect out to host:port. Listener: bind host:port and wait for
        // an encoder to push the stream.
        enum class Mode { Caller, Listener };
        Mode mode = Mode::Caller;
        // In listener mode, the address to bind.
        std::string host = "127.0.0.1";
        uint16_t port = 9000;
        int latencyMs = 120;
        int timeoutMs = 200; // recv timeout for stop-check polling
        // Receive headroom. These cost no added latency (unlike latencyMs) and
        // protect against the receiver briefly falling behind: if the kernel UDP
        // socket or SRT's own buffer overflows, SRT reports loss and drops packets
        // that miss the latency budget, punching a hole in the transport stream.
        // Sized for a 6 Mbit/s CBR feed with plenty of margin; 0 = library default.
        int rcvBufBytes = 8 * 1024 * 1024;    // SRTO_RCVBUF
        int udpRcvBufBytes = 8 * 1024 * 1024; // SRTO_UDP_RCVBUF (kernel socket)
    };

    /// The health of the SRT link being received, as libsrt counts it since
    /// the connection started.
    struct LinkStats {
        double rttMs = 0;
        std::int64_t received = 0;       // packets
        std::int64_t lost = 0;           // packets found missing
        std::int64_t retransmitted = 0;  // packets received again
        std::int64_t dropped = 0;        // packets that came too late to play
        std::int64_t bufferMs = 0;       // media held in the receive buffer
    };

    /// The latest LinkStats: the receive loop stores them once per second,
    /// and any thread reads them.
    class LinkStatsSnapshot {
    public:
        void store(const LinkStats& stats) {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_stats = stats;
        }
        std::optional<LinkStats> load() const {
            std::lock_guard<std::mutex> lock(m_mutex);
            return m_stats;
        }

    private:
        mutable std::mutex m_mutex;
        std::optional<LinkStats> m_stats;
    };

    /// The link figures as the [stats] line shows them.
    static QString formatLinkStats(const LinkStats& stats);

    /// Opens an SRT caller connection to host:port. Blocks until connected or error.
    /// Returns empty string on success, error message on failure.
    static QString connect(const Config& cfg, int& sockOut);

    /// Binds host:port and starts listening (listener mode). Returns empty string
    /// on success, error message on failure.
    static QString listen(const Config& cfg, int& listenSockOut);

    /// Receive loop: reads TS data from the SRT socket and writes it to the
    /// given QIODevice (typically the write end of a pipe). Runs until
    /// stopRequested is set. Call from a worker thread. After a drop, output
    /// resumes at a random access point on resyncPid when it is set and >= 0,
    /// and gaps, when set, records the offset of the gap in the bytes written.
    /// With outputOffset set, the count of bytes written starts from it and is
    /// stored back into it, so one offset can span several connections. With
    /// linkStats set, the link figures are stored into it once per second.
    static void receiveLoop(int sock, int pipeFd, std::atomic<bool>& stopRequested,
                            const std::atomic<int>* resyncPid = nullptr, SourceGapLog* gaps = nullptr,
                            std::uint64_t* outputOffset = nullptr, LinkStatsSnapshot* linkStats = nullptr);

    /// Listener-mode loop: accepts one caller at a time and runs receiveLoop on
    /// it until it disconnects, then accepts the next, until stopRequested.
    /// Everything goes to the same pipe. A caller that follows another starts a
    /// new stretch of the stream, so a gap is recorded at the offset where its
    /// data begins. Call from a worker thread.
    static void listenLoop(int listenSock, int pipeFd, std::atomic<bool>& stopRequested,
                           const std::atomic<int>* resyncPid = nullptr, SourceGapLog* gaps = nullptr,
                           LinkStatsSnapshot* linkStats = nullptr);

    /// Clean up the SRT socket.
    static void close(int sock);

    /// Shut the SRT library down (pairs with the implicit srt_startup() done by
    /// connect()). Call once at process shutdown, after close() and after the
    /// receive thread has been joined. No-op when SRT was never started.
    static void shutdown();
};

} // namespace moq2ts
