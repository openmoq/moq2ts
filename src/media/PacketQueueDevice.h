#pragma once

#include <QByteArray>
#include <QIODevice>

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <utility>

namespace moq2ts {

class SourceGapLog;

// The packets of one program of a live multiplex, between the thread that
// demultiplexes it and the M2tsPacketizer that reads it.
//
// A sequential device that is open for reading from construction on, so an
// M2tsPacketizer uses it as it is: it does not open the device again. push()
// is for the producer thread and never blocks. read() is for the consumer
// thread and blocks until the bytes asked for are queued, finish() was called,
// or abort() was called, so a packetizer that reads 188 or 192 octets gets
// whole packets. peek() goes through the same read, so it also waits for the
// bytes asked for; the packetizer peeks 768 octets to find the packet size.
// After finish(), read() returns what is left and then atEnd()
// is true.
//
// The queue holds capacityBytes. When a push overflows it, the oldest whole
// packets go, and the gap is recorded in the SourceGapLog at the offset in the
// reader's byte stream where the dropped packets would have started, which is
// where the packetizer starts a new Group. A read that ends inside a packet
// (the peek above takes 4 packets and 16 octets of 188-octet packets) keeps
// the rest of that packet queued, so the drop starts at the next packet
// boundary of the reader's stream.
class PacketQueueDevice final : public QIODevice {
public:
    PacketQueueDevice(qsizetype capacityBytes, std::shared_ptr<SourceGapLog> gaps);
    ~PacketQueueDevice() override;

    // The size of the packets that overflow drops, 188 by default.
    void setPacketSize(int bytes);
    // Appends packets. Thread-safe against the reader.
    void push(const QByteArray& packets);
    // Marks a gap in the source before the next byte pushed, with the causes
    // the source gave it (SourceGapLog::Cause). The marker is an offset in the
    // reader's byte stream, so an overflow that drops the bytes queued before
    // it moves it down (to the read offset at most, where the overflow gap
    // is), and it reaches the SourceGapLog no later than the read that returns
    // the byte it precedes. An overflow gap has the cause ProgramQueue.
    void markGap(unsigned causes);
    // Drops everything queued but the rest of a packet the reader has begun,
    // so the reader goes on with the next byte pushed, and records a gap there
    // (cause LiveEdge). Returns the offset of that byte in the reader's
    // stream: the reader read every byte before it, or the start of its
    // packet, before the flush.
    std::uint64_t flushToLiveEdge();
    // No more data: the reader gets what is queued, then the end.
    void finish();
    // Wakes a blocked reader at once, and discards what is queued.
    void abort();
    // Bytes dropped by overflow so far.
    qint64 droppedBytes() const;
    std::shared_ptr<SourceGapLog> gaps() const;

    bool isSequential() const override { return true; }
    qint64 bytesAvailable() const override;
    // Waits up to msecs for a whole packet, the end, or abort(). False on
    // timeout.
    bool waitForReadyRead(int msecs) override;

protected:
    qint64 readData(char* data, qint64 maxSize) override;
    qint64 writeData(const char*, qint64) override { return -1; }

private:
    // Octets of the packet the reader has begun that it has not read yet.
    // Under m_mutex.
    qsizetype partialPacket() const;

    const qsizetype m_capacity;
    const std::shared_ptr<SourceGapLog> m_gaps;
    int m_packetSize = 188;

    mutable std::mutex m_mutex;
    std::condition_variable m_wake;
    QByteArray m_queue;          // m_queue[m_head..] is what is queued
    qsizetype m_head = 0;
    std::uint64_t m_readOffset = 0;   // bytes the reader has taken
    qint64 m_dropped = 0;
    // Gaps not yet in m_gaps: offsets in the reader's stream, and causes.
    std::deque<std::pair<std::uint64_t, unsigned>> m_markers;
    bool m_finished = false;
    bool m_aborted = false;
};

} // namespace moq2ts
