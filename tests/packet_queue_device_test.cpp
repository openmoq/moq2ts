#include "media/M2tsPacketizer.h"
#include "media/PacketQueueDevice.h"
#include "media/SourceGapLog.h"
#include "ts_test_builder.h"

#include <QFile>
#include <QTemporaryDir>

#include <sys/stat.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

using moq2ts::M2tsObject;
using moq2ts::M2tsPacketizer;
using moq2ts::PacketQueueDevice;
using moq2ts::SourceGapLog;
namespace tb = moq2ts::test;

namespace {
bool expect(bool cond, const std::string& msg) {
    if (!cond) { std::cerr << "FAIL: " << msg << '\n'; return false; }
    return true;
}

constexpr int kPacket = 188;

// Packet number n: a TS packet whose payload is filled with the byte n.
QByteArray numbered(int n) {
    return tb::tsPacket(0x100, false, QByteArray(184, static_cast<char>(n)));
}

QByteArray numbered(int first, int count) {
    QByteArray bytes;
    for (int n = first; n < first + count; ++n) {
        bytes += numbered(n);
    }
    return bytes;
}

// PAT, PMT, a random access point, then plain video packets.
QByteArray liveStream() {
    QByteArray ts = tb::psiPacket(0x0000, tb::patSection({{1, 0x1000}}));
    ts += tb::psiPacket(0x1000, tb::pmtSection(1, 0x100, {{0x1B, 0x100}}));
    ts += tb::tsPacket(0x100, true, tb::pesHeaderWithPts(9000), 0, 0x40);
    for (int cc = 1; cc <= 6; ++cc) {
        ts += tb::tsPacket(0x100, false, QByteArray(184, char(0x00)), cc);
    }
    return ts;
}

// An adaptation-only packet on PID 0x100 (the PCR PID) with a PCR of base
// ticks of 90 kHz.
QByteArray pcrPacket(std::uint64_t base, int continuityCounter) {
    QByteArray packet;
    packet.append(char(0x47));
    packet.append(char(0x01));
    packet.append(char(0x00));
    packet.append(static_cast<char>(0x20 | (continuityCounter & 0x0F)));
    packet.append(char(183));    // adaptation_field_length
    packet.append(char(0x10));   // PCR_flag
    packet.append(static_cast<char>(base >> 25));
    packet.append(static_cast<char>(base >> 17));
    packet.append(static_cast<char>(base >> 9));
    packet.append(static_cast<char>(base >> 1));
    packet.append(static_cast<char>(((base & 1) << 7) | 0x7E));
    packet.append(char(0x00));
    packet.append(QByteArray(188 - packet.size(), char(0xFF)));
    return packet;
}

// liveStream(), then 1.1 s of PCRs.
QByteArray liveStreamWithPcr() {
    QByteArray ts = liveStream();
    for (int n = 0; n <= 11; ++n) {
        ts += pcrPacket(9000 * static_cast<std::uint64_t>(n), 6);
    }
    return ts;
}

QList<QByteArray> publishObjects(M2tsPacketizer& packetizer, QString* error) {
    QList<QByteArray> payloads;
    M2tsObject object;
    while (packetizer.readObject(1, &object, error)) {
        payloads.append(object.payload);
    }
    return payloads;
}

// The source stays open and quiet after its packets: readObject must return
// the partial Object after about 50 ms. endSource is the guard, which ends a
// read that is still waiting after 2 s.
bool returnsPartialObject(M2tsPacketizer& packetizer, const std::function<void()>& endSource, const std::string& name) {
    using Clock = std::chrono::steady_clock;
    M2tsObject object;
    QString error;
    bool read = false;
    std::atomic<bool> done{false};
    const Clock::time_point start = Clock::now();
    Clock::time_point end;
    std::thread reader([&] {
        read = packetizer.readObject(348, &object, &error);
        end = Clock::now();
        done = true;
    });
    while (!done && Clock::now() - start < std::chrono::seconds(2)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    const bool inTime = done;
    endSource();
    reader.join();
    bool ok = expect(inTime, name + ": the read returns while the source is quiet");
    if (inTime) {
        const auto held = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
        ok &= expect(read && !object.payload.isEmpty() && object.payload.size() < 348 * kPacket,
                     name + ": a partial Object: " + error.toStdString());
        ok &= expect(held >= 50, name + ": held " + std::to_string(held) + " ms, at least 50");
    }
    return ok;
}
}  // namespace

int main() {
    bool ok = true;

    // Pushed bytes read back in order, whole packets at a time.
    {
        PacketQueueDevice device(10 * kPacket, std::make_shared<SourceGapLog>());
        device.push(numbered(0, 2));
        device.push(numbered(2, 1));
        ok &= expect(device.isSequential() && device.isOpen(), "queue is an open sequential device");
        ok &= expect(device.read(kPacket) == numbered(0), "first packet");
        ok &= expect(device.read(kPacket) == numbered(1), "second packet");
        ok &= expect(device.read(kPacket) == numbered(2), "third packet");
        ok &= expect(device.droppedBytes() == 0, "nothing dropped");
    }

    // peek returns the next bytes without consuming them.
    {
        PacketQueueDevice device(10 * kPacket, std::make_shared<SourceGapLog>());
        device.push(numbered(0, 5));
        ok &= expect(device.peek(192 * 4) == numbered(0, 5).left(192 * 4), "peek sees the queued bytes");
        ok &= expect(device.peek(kPacket) == numbered(0), "peek returns the first packet");
        ok &= expect(device.read(kPacket) == numbered(0), "peek consumed nothing");
    }

    // Overflow drops the oldest whole packets, never blocks, and logs one gap.
    {
        auto gaps = std::make_shared<SourceGapLog>();
        PacketQueueDevice device(10 * kPacket, gaps);
        device.push(numbered(0, 4));
        ok &= expect(device.read(4 * kPacket) == numbered(0, 4), "read the first four");
        device.push(numbered(4, 15));
        ok &= expect(device.droppedBytes() == 5 * kPacket, "five packets dropped");
        ok &= expect(gaps->next() == std::optional<std::uint64_t>(4 * kPacket), "one gap at the read offset");
        ok &= expect(device.read(10 * kPacket) == numbered(9, 10), "the newest ten packets remain");
        ok &= expect(gaps->takeUpTo(4 * kPacket) && !gaps->next().has_value(), "only one gap");
        device.push(numbered(19, 1));
        ok &= expect(device.droppedBytes() == 5 * kPacket && !gaps->next().has_value(), "no drop once drained");
    }

    // finish() lets the reader take the rest, then atEnd().
    {
        PacketQueueDevice device(10 * kPacket, std::make_shared<SourceGapLog>());
        device.push(numbered(0, 3));
        device.finish();
        ok &= expect(!device.atEnd(), "data is left after finish");
        ok &= expect(device.read(10 * kPacket) == numbered(0, 3), "the rest after finish");
        ok &= expect(device.atEnd(), "atEnd after the rest");
        ok &= expect(device.read(kPacket).isEmpty(), "no more data");
    }

    // A read blocks until a producer thread pushes, and returns whole packets.
    {
        PacketQueueDevice device(10 * kPacket, std::make_shared<SourceGapLog>());
        std::thread producer([&] {
            for (int n = 0; n < 5; ++n) {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                device.push(numbered(n));
            }
        });
        const QByteArray got = device.read(5 * kPacket);
        producer.join();
        ok &= expect(got == numbered(0, 5), "a blocking read waits for all five packets");
    }

    // abort() wakes a blocked reader.
    {
        PacketQueueDevice device(10 * kPacket, std::make_shared<SourceGapLog>());
        std::atomic<bool> returned{false};
        std::thread reader([&] {
            device.read(kPacket);
            returned = true;
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        ok &= expect(!returned, "the reader blocks on an empty queue");
        device.abort();
        reader.join();
        ok &= expect(returned, "abort wakes the reader");
    }

    // A packetizer on a queue publishes the same Objects as on a FIFO, the other
    // live source: both drop the lead-in before the first random access point.
    {
        const QByteArray ts = liveStream();
        QTemporaryDir dir;
        const QString fifo = dir.filePath("live.fifo");
        ::mkfifo(fifo.toLocal8Bit().constData(), 0600);
        std::thread writer([&] {
            QFile file(fifo);
            if (file.open(QIODevice::WriteOnly)) {
                file.write(ts);
            }
        });
        QString fileError;
        M2tsPacketizer fromFifo(fifo);
        ok &= expect(fromFifo.open(1, &fileError), "FIFO opens: " + fileError.toStdString());
        const QList<QByteArray> expected = publishObjects(fromFifo, &fileError);
        writer.join();

        auto device = std::make_shared<PacketQueueDevice>(100 * kPacket, std::make_shared<SourceGapLog>());
        std::thread producer([&] {
            for (int offset = 0; offset < ts.size(); offset += kPacket) {
                device->push(ts.mid(offset, kPacket));
            }
            device->finish();
        });
        QString queueError;
        M2tsPacketizer fromQueue(device, QStringLiteral("program 1"));
        ok &= expect(fromQueue.open(1, &queueError), "queue opens: " + queueError.toStdString());
        ok &= expect(fromQueue.sequential(), "the queue takes the live path");
        const QList<QByteArray> got = publishObjects(fromQueue, &queueError);
        producer.join();
        ok &= expect(!expected.isEmpty() && got == expected, "same Objects from the queue as from the FIFO");
    }

    // A live source that goes quiet does not hold the packets already read:
    // the partial Object goes out, on a FIFO and on a queue. The PCRs end the
    // 1 s window of open(), which then needs no more packets.
    {
        QTemporaryDir dir;
        const QString fifo = dir.filePath("quiet.fifo");
        ::mkfifo(fifo.toLocal8Bit().constData(), 0600);
        std::atomic<bool> closeWriter{false};
        std::thread writer([&] {
            QFile file(fifo);
            if (file.open(QIODevice::WriteOnly | QIODevice::Unbuffered)) {
                file.write(liveStreamWithPcr());
                while (!closeWriter) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                }
            }
        });
        QString error;
        M2tsPacketizer fromFifo(fifo);
        ok &= expect(fromFifo.open(1, &error), "quiet FIFO opens: " + error.toStdString());
        ok &= returnsPartialObject(fromFifo, [&] { closeWriter = true; }, "quiet FIFO");
        closeWriter = true;
        writer.join();
    }
    {
        auto device = std::make_shared<PacketQueueDevice>(100 * kPacket, std::make_shared<SourceGapLog>());
        device->push(liveStreamWithPcr());
        QString error;
        M2tsPacketizer fromQueue(device, QStringLiteral("program 1"));
        ok &= expect(fromQueue.open(1, &error), "quiet queue opens: " + error.toStdString());
        ok &= returnsPartialObject(fromQueue, [&] { device->finish(); }, "quiet queue");
    }

    // A gap marked at the next byte pushed stays at that byte when an overflow
    // drops the packets before it, the overflow gap is not lost behind it, and
    // the marker reaches the log with the read of the byte after it.
    {
        auto gaps = std::make_shared<SourceGapLog>();
        PacketQueueDevice device(10 * kPacket, gaps);
        device.push(numbered(0, 2));
        ok &= expect(device.read(2 * kPacket) == numbered(0, 2), "gap then overflow: read two");
        device.push(numbered(2, 4));
        device.markGap(SourceGapLog::SrtReceiveBuffer);                 // at 2 + 4 = 6 packets
        ok &= expect(!gaps->next().has_value(), "gap then overflow: nothing published before the read");
        device.push(numbered(6, 8));            // 12 queued: drops 2 packets
        ok &= expect(gaps->next() == std::optional<std::uint64_t>(2 * kPacket), "gap then overflow: overflow gap at the read offset");
        ok &= expect(device.read(2 * kPacket) == numbered(4, 2), "gap then overflow: read to the gap");
        ok &= expect(gaps->takeUpTo(2 * kPacket) && gaps->next() == std::optional<std::uint64_t>(4 * kPacket),
                     "gap then overflow: the marker moved down by the dropped bytes");
        ok &= expect(gaps->takeUpTo(4 * kPacket) && !gaps->next().has_value(), "gap then overflow: two gaps");
    }
    // Overflow, then a gap: the marker is at the offset of the next byte.
    {
        auto gaps = std::make_shared<SourceGapLog>();
        PacketQueueDevice device(4 * kPacket, gaps);
        device.push(numbered(0, 6));            // drops 2 packets, overflow gap at 0 (ignored)
        device.markGap(SourceGapLog::SrtReceiveBuffer);                 // at 4 packets in the reader's stream
        device.push(numbered(6, 1));            // drops 1 more: marker at 3 packets
        ok &= expect(device.read(3 * kPacket) == numbered(3, 3), "overflow then gap: read up to the gap");
        ok &= expect(gaps->next() == std::optional<std::uint64_t>(3 * kPacket), "overflow then gap: marker at the next byte");
        ok &= expect(gaps->takeUpTo(3 * kPacket) && !gaps->next().has_value(), "overflow then gap: one gap");
    }
    // A marker inside the dropped region collapses into the overflow gap.
    {
        auto gaps = std::make_shared<SourceGapLog>();
        PacketQueueDevice device(4 * kPacket, gaps);
        device.push(numbered(0, 3));
        ok &= expect(device.read(2 * kPacket) == numbered(0, 2), "collapse: read two");
        device.markGap(SourceGapLog::SrtReceiveBuffer);                 // at 3 packets, the byte after packet 2
        device.push(numbered(3, 6));            // 7 queued: drops 3 packets, past the marker
        ok &= expect(gaps->next() == std::optional<std::uint64_t>(2 * kPacket), "collapse: overflow gap at the read offset");
        ok &= expect(device.read(kPacket) == numbered(5, 1), "collapse: read after the drop");
        ok &= expect(gaps->takeUpTo(100 * kPacket) && !gaps->next().has_value(), "collapse: one gap, not two");
    }

    // Each gap says which hop skipped ahead: an overflow is this queue, a
    // marker keeps the cause the source gave, and the two merge where a marker
    // collapses into an overflow gap.
    {
        auto gaps = std::make_shared<SourceGapLog>();
        PacketQueueDevice device(4 * kPacket, gaps);
        device.push(numbered(0, 3));
        device.read(2 * kPacket);
        device.markGap(SourceGapLog::SrtReceiveBuffer);   // at 3 packets
        device.push(numbered(3, 3));                      // 4 queued: no drop
        device.read(2 * kPacket);                         // reaches the marker
        unsigned causes = 0;
        ok &= expect(gaps->takeUpTo(3 * kPacket, &causes) && causes == SourceGapLog::SrtReceiveBuffer,
                     "causes: a marker keeps the source's cause");
        device.push(numbered(6, 4));                      // 6 queued: drops 2, overflow gap at 4 packets
        causes = 0;
        ok &= expect(gaps->takeUpTo(4 * kPacket, &causes) && causes == SourceGapLog::ProgramQueue,
                     "causes: an overflow gap names the program queue");
        device.read(kPacket);
        device.markGap(SourceGapLog::SrtReceiveBuffer);   // at 5 + 3 = 8 packets
        device.push(numbered(10, 4));                     // 7 queued: drops 3, past the marker
        device.read(kPacket);
        causes = 0;
        ok &= expect(gaps->takeUpTo(100 * kPacket, &causes) &&
                         causes == (SourceGapLog::SrtReceiveBuffer | SourceGapLog::ProgramQueue),
                     "causes: a collapsed marker merges its cause into the overflow gap");
    }

    // The live edge: a flush drops what is queued, returns the read offset,
    // and records a gap there that a pending marker joins.
    {
        auto gaps = std::make_shared<SourceGapLog>();
        PacketQueueDevice device(10 * kPacket, gaps);
        device.push(numbered(0, 3));
        device.read(kPacket);
        device.markGap(SourceGapLog::SrtReceiveBuffer);   // at 3 packets
        device.push(numbered(3, 2));
        ok &= expect(device.flushToLiveEdge() == kPacket, "flush: returns the read offset");
        ok &= expect(device.bytesAvailable() == 0, "flush: nothing queued");
        device.push(numbered(5, 1));
        ok &= expect(device.read(kPacket) == numbered(5, 1), "flush: the next byte pushed comes next");
        unsigned causes = 0;
        ok &= expect(gaps->takeUpTo(kPacket, &causes) &&
                         causes == (SourceGapLog::LiveEdge | SourceGapLog::SrtReceiveBuffer) && !gaps->next(),
                     "flush: one gap at the read offset, with both causes");
    }

    // The packetizer's peek of 768 octets takes 4 packets and 16 octets of the
    // fifth from the queue. An overflow then keeps the rest of the fifth, so
    // every packet read is a source packet, and the gap is where the dropped
    // packets would have started.
    {
        auto gaps = std::make_shared<SourceGapLog>();
        PacketQueueDevice device(10 * kPacket, gaps);
        device.push(numbered(0, 6));
        ok &= expect(device.peek(192 * 4) == numbered(0, 6).left(192 * 4), "peek overflow: peek 768");
        device.push(numbered(6, 10));   // 11 packets and 172 octets queued: drops 2 packets
        device.finish();                // a short read returns rather than blocks
        ok &= expect(device.droppedBytes() == 2 * kPacket, "peek overflow: two packets dropped");
        for (int n = 0; n < 5; ++n) {
            ok &= expect(device.read(kPacket) == numbered(n), "peek overflow: packet " + std::to_string(n) + " intact");
        }
        ok &= expect(gaps->next() == std::optional<std::uint64_t>(5 * kPacket), "peek overflow: gap after the fifth");
        for (int n = 7; n < 16; ++n) {
            ok &= expect(device.read(kPacket) == numbered(n), "peek overflow: packet " + std::to_string(n) + " intact");
        }
    }
    // The same for the live-edge flush: it keeps the rest of the fifth packet,
    // and the next byte pushed starts a packet.
    {
        auto gaps = std::make_shared<SourceGapLog>();
        PacketQueueDevice device(10 * kPacket, gaps);
        device.push(numbered(0, 6));
        device.peek(192 * 4);
        ok &= expect(device.flushToLiveEdge() == 5 * kPacket, "peek flush: returns the offset of the next packet");
        device.push(numbered(10, 2));
        device.finish();
        for (const int n : {0, 1, 2, 3, 4, 10, 11}) {
            ok &= expect(device.read(kPacket) == numbered(n), "peek flush: packet " + std::to_string(n) + " intact");
        }
        unsigned causes = 0;
        ok &= expect(gaps->takeUpTo(5 * kPacket, &causes) && causes == SourceGapLog::LiveEdge && !gaps->next(),
                     "peek flush: one gap after the fifth packet");
    }

    ok &= expect(moq2ts::detectTsPacketSize(numbered(0, 4)) == 188, "detect 188");
    ok &= expect(moq2ts::detectTsPacketSize(QByteArray(10, char(0))) == 0, "detect nothing");
    return ok ? 0 : 1;
}
