#include "PacketQueueDevice.h"

#include "SourceGapLog.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <utility>

namespace moq2ts {

PacketQueueDevice::PacketQueueDevice(qsizetype capacityBytes, std::shared_ptr<SourceGapLog> gaps)
    : m_capacity(capacityBytes), m_gaps(std::move(gaps)) {
    // Unbuffered: QIODevice then asks readData for exactly the bytes wanted,
    // not for a read-ahead chunk that would block until it was full.
    open(QIODevice::ReadOnly | QIODevice::Unbuffered);
}

PacketQueueDevice::~PacketQueueDevice() = default;

void PacketQueueDevice::setPacketSize(int bytes) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_packetSize = bytes;
}

void PacketQueueDevice::push(const QByteArray& packets) {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_aborted || m_finished) {
            return;
        }
        m_queue.append(packets);
        const qsizetype queued = m_queue.size() - m_head;
        if (queued > m_capacity) {
            // Whole packets only: round the excess up to the packet size, and
            // keep the rest of a packet the reader has begun, so it stays on
            // a packet boundary.
            const qsizetype partial = std::min(queued, partialPacket());
            const qsizetype excess = queued - m_capacity;
            const qsizetype drop =
                std::min(queued - partial, (excess + m_packetSize - 1) / m_packetSize * m_packetSize);
            std::memmove(m_queue.data() + m_head + drop, m_queue.constData() + m_head, partial);
            m_head += drop;
            m_dropped += drop;
            // The bytes after the dropped ones now start after that rest.
            const std::uint64_t gap = m_readOffset + static_cast<std::uint64_t>(partial);
            for (auto& [marker, causes] : m_markers) {
                marker = marker > gap + static_cast<std::uint64_t>(drop) ? marker - drop : gap;
            }
            if (m_gaps) {
                m_gaps->record(gap, SourceGapLog::ProgramQueue);
            }
        }
        // Reclaim the consumed front once it is at least as large as the rest.
        if (m_head > 0 && m_head >= m_queue.size() - m_head) {
            m_queue.remove(0, m_head);
            m_head = 0;
        }
    }
    m_wake.notify_all();
}

void PacketQueueDevice::markGap(unsigned causes) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_markers.emplace_back(m_readOffset + static_cast<std::uint64_t>(m_queue.size() - m_head), causes);
}

std::uint64_t PacketQueueDevice::flushToLiveEdge() {
    std::lock_guard<std::mutex> lock(m_mutex);
    // The rest of a packet the reader has begun stays, as in overflow.
    const qsizetype partial = std::min(m_queue.size() - m_head, partialPacket());
    m_queue = m_queue.mid(m_head, partial);
    m_head = 0;
    const std::uint64_t edge = m_readOffset + static_cast<std::uint64_t>(partial);
    // Gaps in the dropped bytes now fall at the edge, as in overflow.
    for (auto& [marker, causes] : m_markers) {
        marker = edge;
    }
    if (m_gaps) {
        m_gaps->record(edge, SourceGapLog::LiveEdge);
    }
    return edge;
}

qsizetype PacketQueueDevice::partialPacket() const {
    const auto begun = static_cast<qsizetype>(m_readOffset % static_cast<std::uint64_t>(m_packetSize));
    return begun == 0 ? 0 : m_packetSize - begun;
}

void PacketQueueDevice::finish() {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_finished = true;
    }
    m_wake.notify_all();
}

void PacketQueueDevice::abort() {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_aborted = true;
        m_queue.clear();
        m_head = 0;
    }
    m_wake.notify_all();
}

qint64 PacketQueueDevice::droppedBytes() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_dropped;
}

std::shared_ptr<SourceGapLog> PacketQueueDevice::gaps() const {
    return m_gaps;
}

qint64 PacketQueueDevice::bytesAvailable() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return (m_queue.size() - m_head) + QIODevice::bytesAvailable();
}

bool PacketQueueDevice::waitForReadyRead(int msecs) {
    std::unique_lock<std::mutex> lock(m_mutex);
    return m_wake.wait_for(lock, std::chrono::milliseconds(msecs), [&] {
        return m_aborted || m_finished || m_queue.size() - m_head >= m_packetSize;
    });
}

qint64 PacketQueueDevice::readData(char* data, qint64 maxSize) {
    std::unique_lock<std::mutex> lock(m_mutex);
    // Wait for the whole request, so that a reader of packets never gets half
    // of one, unless no more data will come. A request over the capacity
    // could never be met, so it waits for a full queue.
    const qsizetype wanted = static_cast<qsizetype>(std::min<qint64>(maxSize, m_capacity));
    m_wake.wait(lock, [&] {
        return m_aborted || m_finished || m_queue.size() - m_head >= wanted;
    });
    const qint64 count = std::min<qint64>(maxSize, m_queue.size() - m_head);
    if (count <= 0) {
        return -1;   // the end of a sequential device
    }
    std::copy_n(m_queue.constData() + m_head, count, data);
    m_head += count;
    m_readOffset += static_cast<std::uint64_t>(count);
    // Markers are in increasing order. Publish those the bytes just returned
    // reach, so the reader sees the gap before it handles the byte after it.
    while (!m_markers.empty() && m_markers.front().first <= m_readOffset) {
        if (m_gaps) {
            m_gaps->record(m_markers.front().first, m_markers.front().second);
        }
        m_markers.pop_front();
    }
    return count;
}

} // namespace moq2ts
