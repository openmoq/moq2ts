#include "media/M2tsPacketizer.h"
#include "media/SourceGapLog.h"
#include "ts_test_builder.h"

#include <QDir>
#include <QFile>
#include <QMap>
#include <QSet>
#include <QStringList>
#include <QtGlobal>
#include <QTemporaryDir>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <chrono>
#include <csignal>

#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <atomic>

using moq2ts::M2tsObject;
using moq2ts::M2tsPacketizer;
using moq2ts::SourceGapLog;
namespace tb = moq2ts::test;

namespace {
bool expect(bool cond, const std::string& msg) {
    if (!cond) { std::cerr << "FAIL: " << msg << '\n'; return false; }
    return true;
}

bool writeFile(const QString& path, const QByteArray& data) {
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(data) == data.size();
}

// PAT, one PMT per program, then video packets on PID 0x100.
QByteArray stream(const QList<std::pair<int, int>>& programs) {
    QByteArray ts = tb::psiPacket(0x0000, tb::patSection(programs));
    for (const auto& [program, pmtPid] : programs) {
        ts += tb::psiPacket(pmtPid, tb::pmtSection(program, 0x100, {{0x1B, 0x100}, {0x0F, 0x101}}));
    }
    for (int cc = 0; cc < 8; ++cc) {
        ts += tb::tsPacket(0x100, false, QByteArray(184, char(0x00)), cc);
    }
    return ts;
}
// Qt warnings logged since the last call to takeWarnings().
QStringList& warnings() {
    static QStringList list;
    return list;
}

void captureWarning(QtMsgType type, const QMessageLogContext&, const QString& message) {
    if (type == QtWarningMsg) {
        warnings().append(message);
    }
}

// Every packet the packetizer publishes, and the error that ends the track.
struct Run {
    QList<QByteArray> packets;
    QString error;
};

Run publish(const QString& path, bool transparent, int program = 0, bool retainSi = false) {
    Run run;
    M2tsPacketizer packetizer(path);
    packetizer.setTransparent(transparent);
    packetizer.setRetainSiTables(retainSi);
    if (!packetizer.open(program, &run.error)) {
        return run;
    }
    M2tsObject object;
    while (packetizer.readObject(1, &object, &run.error)) {
        run.packets.append(object.payload);
    }
    return run;
}

int pidOfPacket(const QByteArray& packet) {
    return ((static_cast<unsigned char>(packet[1]) & 0x1F) << 8) | static_cast<unsigned char>(packet[2]);
}

QList<int> pids(const Run& run) {
    QList<int> result;
    for (const QByteArray& packet : run.packets) {
        result.append(pidOfPacket(packet));
    }
    return result;
}
// A packet on pid that carries a PCR (27 MHz) in its adaptation field.
QByteArray pcrPacket(int pid, std::int64_t pcr, int cc) {
    const std::int64_t base = pcr / 300;
    const int extension = static_cast<int>(pcr % 300);
    QByteArray packet;
    packet.append(char(0x47));
    packet.append(static_cast<char>((pid >> 8) & 0x1F));
    packet.append(static_cast<char>(pid & 0xFF));
    packet.append(static_cast<char>(0x30 | (cc & 0x0F)));
    packet.append(char(7));                                    // adaptation_field_length
    packet.append(char(0x10));                                 // PCR_flag
    packet.append(static_cast<char>((base >> 25) & 0xFF));
    packet.append(static_cast<char>((base >> 17) & 0xFF));
    packet.append(static_cast<char>((base >> 9) & 0xFF));
    packet.append(static_cast<char>((base >> 1) & 0xFF));
    packet.append(static_cast<char>(((base & 0x01) << 7) | 0x7E | ((extension >> 8) & 0x01)));
    packet.append(static_cast<char>(extension & 0xFF));
    packet.append(QByteArray(188 - packet.size(), char(0)));
    return packet;
}

// Opens path as a live source: a FIFO that a second thread fills with data.
struct LiveRun {
    Run run;
    bool randomAccess = false;
    QByteArray initData;
    bool firstStartsGroup = false;
};

LiveRun publishLive(const QString& fifo, const QByteArray& data, bool transparent, int program = 0) {
    LiveRun live;
    ::mkfifo(fifo.toLocal8Bit().constData(), 0600);
    std::thread writer([&] { writeFile(fifo, data); });
    M2tsPacketizer packetizer(fifo);
    packetizer.setTransparent(transparent);
    if (packetizer.open(program, &live.run.error)) {
        live.randomAccess = packetizer.randomAccess();
        live.initData = packetizer.initData();
        M2tsObject object;
        while (packetizer.readObject(1, &object, &live.run.error)) {
            if (live.run.packets.isEmpty()) {
                live.firstStartsGroup = object.startsGroup && object.groupId == 0;
            }
            live.run.packets.append(object.payload);
        }
    }
    writer.join();
    return live;
}
}  // namespace

// Opens a live FIFO that carries ts with timed start-up windows, as a program
// of a multiplex does. Returns whether it opened, and fills randomAccess and,
// when given, the measured bitrate and mux rate.
bool timedOpen(const QString& fifo, const QByteArray& ts, int program, bool* randomAccess, QString* error,
               qint64* measuredBitrate = nullptr, qint64* measuredMuxRate = nullptr) {
    ::mkfifo(fifo.toLocal8Bit().constData(), 0600);
    std::thread writer([&] {
        const int fd = ::open(fifo.toLocal8Bit().constData(), O_WRONLY);
        if (fd >= 0) {
            qsizetype done = 0;
            while (done < ts.size()) {
                const ssize_t n = ::write(fd, ts.constData() + done, ts.size() - done);
                if (n <= 0) {
                    break;
                }
                done += n;
            }
            ::close(fd);
        }
    });
    bool opened = false;
    {
        M2tsPacketizer packetizer(fifo);
        packetizer.setTimedStartWindows(true);
        opened = packetizer.open(program, error);
        *randomAccess = opened && packetizer.randomAccess();
        if (measuredBitrate) {
            *measuredBitrate = packetizer.measuredBitrate();
        }
        if (measuredMuxRate) {
            *measuredMuxRate = packetizer.measuredMuxRate();
        }
    }
    writer.join();
    return opened;
}

int main() {
    std::signal(SIGPIPE, SIG_IGN);
    bool ok = true;
    QTemporaryDir dir;
    ok &= expect(dir.isValid(), "temporary directory");

    const QString spts = dir.filePath("spts.ts");
    const QString mpts = dir.filePath("mpts.ts");
    // program_number 0 is the network PID and does not count as a program.
    ok &= expect(writeFile(spts, stream({{0, 0x10}, {1, 0x1000}})), "write SPTS");
    ok &= expect(writeFile(mpts, stream({{1, 0x1000}, {2, 0x1001}})), "write MPTS");

    // Transparent scan: the program count decides between the unmodified modes.
    for (const bool transparent : {true, false}) {
        const std::string label = transparent ? "transparent" : "filtered";
        M2tsPacketizer single(spts);
        single.setTransparent(transparent);
        QString error;
        ok &= expect(single.open(0, &error), label + " SPTS opens: " + error.toStdString());
        ok &= expect(single.patProgramCount() == 1, label + " SPTS lists one program");
        ok &= expect(single.programNumber() == 1, label + " SPTS program number");

        M2tsPacketizer multiple(mpts);
        multiple.setTransparent(transparent);
        ok &= expect(multiple.open(0, &error), label + " MPTS opens: " + error.toStdString());
        ok &= expect(multiple.patProgramCount() == 2, label + " MPTS lists two programs");
    }

    // A transparent SPTS ignores a requested program number that it does not carry.
    {
        M2tsPacketizer single(spts);
        single.setTransparent(true);
        QString error;
        ok &= expect(single.open(7, &error), "transparent SPTS opens with --program 7");
        ok &= expect(single.programNumber() == 1 && single.pcrPid() == 0x100,
                     "transparent SPTS describes its own program");
    }

    // initData holds every packet of a PMT that spans several packets.
    {
        QList<std::pair<int, int>> streams{{0x1B, 0x100}};
        for (int index = 0; index < 99; ++index) {
            streams.append({0x06, 0x200 + index});
        }
        const QByteArray pat = tb::psiPacket(0x0000, tb::patSection({{1, 0x1000}}));
        const QList<QByteArray> pmt = tb::psiPackets(0x1000, tb::pmtSection(1, 0x100, streams));
        QByteArray ts = pat;
        for (const QByteArray& packet : pmt) {
            ts += packet;
        }
        ts += tb::tsPacket(0x100, false, QByteArray(184, char(0)));
        const QString path = dir.filePath("long-pmt.ts");
        ok &= expect(writeFile(path, ts) && pmt.size() == 3, "write long-PMT stream");
        M2tsPacketizer packetizer(path);
        QString error;
        ok &= expect(packetizer.open(0, &error), "long PMT opens: " + error.toStdString());
        ok &= expect(packetizer.initData() == ts.left(4 * 188), "initData holds the PAT and all three PMT packets");
    }

    // Per-program carriage rewrites the PAT: one packet per source PAT, listing
    // the selected program only, with its own version_number and continuity
    // counter. The network PID entry stays only with --retain-si.
    {
        // 60 programs plus the network PID: 255 octets, two packets.
        QList<std::pair<int, int>> programs{{0, 0x0010}};
        for (int program = 1; program <= 60; ++program) {
            programs.append({program, 0x1000 + program});
        }
        const QByteArray patSection = tb::patSection(programs);
        const QByteArray pmt = tb::psiPacket(0x1002, tb::pmtSection(2, 0x200, {{0x1B, 0x200}}));
        QByteArray ts;
        int patCc = 0;
        for (int repeat = 0; repeat < 3; ++repeat) {
            for (const QByteArray& packet : tb::psiPackets(0x0000, patSection, patCc)) {
                ts += packet;
                patCc = (patCc + 1) & 0x0F;
            }
            ts += pmt + tb::tsPacket(0x200, false, QByteArray(184, char(0)), repeat);
        }
        const QString path = dir.filePath("long-pat.ts");
        ok &= expect(writeFile(path, ts) && tb::psiPackets(0, patSection).size() == 2, "write long-PAT stream");

        for (const bool retainSi : {false, true}) {
            const std::string label = retainSi ? "PAT rewrite with --retain-si" : "PAT rewrite";
            M2tsPacketizer packetizer(path);
            packetizer.setRetainSiTables(retainSi);
            QString error;
            ok &= expect(packetizer.open(2, &error), label + ": opens: " + error.toStdString());
            QByteArray out;
            M2tsObject object;
            while (packetizer.readObject(4, &object, &error)) {
                out += object.payload;
            }
            QList<QByteArray> patPackets;
            for (qsizetype offset = 0; offset + 188 <= out.size(); offset += 188) {
                if ((((static_cast<unsigned char>(out[offset + 1]) & 0x1F) << 8) | static_cast<unsigned char>(out[offset + 2])) == 0) {
                    patPackets.append(out.mid(offset, 188));
                }
            }
            ok &= expect(patPackets.size() == 3, label + ": one packet per source PAT");
            QByteArray expected = tb::patSection(retainSi ? QList<std::pair<int, int>>{{0, 0x0010}, {2, 0x1002}}
                                                          : QList<std::pair<int, int>>{{2, 0x1002}});
            moq2ts::PsiAssembler assembler;
            for (int index = 0; index < patPackets.size(); ++index) {
                ok &= expect((static_cast<unsigned char>(patPackets.at(index)[3]) & 0x0F) == index,
                             label + ": continuous continuity counter");
                const auto sections = assembler.push(patPackets.at(index), patPackets.at(index));
                ok &= expect(sections.size() == 1 && sections.at(0).bytes == expected,
                             label + ": the selected program only, version 0, valid CRC_32");
            }
            ok &= expect(packetizer.initData().left(188) == patPackets.value(0), label + ": initData starts with the rewritten PAT");
        }
    }

    // Live PSI tracking: a PMT that adds a PID. The new PID is dropped before
    // the change and kept after it.
    {
        const QByteArray pat = tb::psiPacket(0x0000, tb::patSection({{1, 0x1000}}));
        const QByteArray video = tb::tsPacket(0x100, false, QByteArray(184, char(0)));
        const QByteArray audio = tb::tsPacket(0x101, false, QByteArray(184, char(0)));
        QByteArray ts = pat + tb::psiPacket(0x1000, tb::pmtSection(1, 0x100, {{0x1B, 0x100}}, 0)) + video + audio;
        ts += tb::psiPacket(0x1000, tb::pmtSection(1, 0x100, {{0x1B, 0x100}, {0x0F, 0x101}}, 1), 1) + video + audio;
        const QString path = dir.filePath("pmt-adds-pid.ts");
        ok &= expect(writeFile(path, ts), "write PMT change stream");
        const Run run = publish(path, false);
        ok &= expect(pids(run) == QList<int>({0x0000, 0x1000, 0x100, 0x1000, 0x100, 0x101}),
                     "PMT change: the new audio PID is kept after the change only");
    }

    // A PMT change gives new initData once; a repeated PMT does not. In
    // per-program carriage it starts with the rewritten PAT, in
    // unmodified-program carriage with the source PAT.
    for (const bool transparent : {false, true}) {
        const std::string label = transparent ? "unmodified-program initData change" : "per-program initData change";
        const QByteArray pat = tb::psiPacket(0x0000, tb::patSection({{1, 0x1000}}));
        const QByteArray pmt0 = tb::psiPacket(0x1000, tb::pmtSection(1, 0x100, {{0x1B, 0x100}}, 0), 0);
        const QByteArray pmt0Again = tb::psiPacket(0x1000, tb::pmtSection(1, 0x100, {{0x1B, 0x100}}, 0), 1);
        const QByteArray pmt1 = tb::psiPacket(0x1000, tb::pmtSection(1, 0x100, {{0x1B, 0x100}, {0x0F, 0x101}}, 1), 2);
        const QByteArray video = tb::tsPacket(0x100, false, QByteArray(184, char(0)));
        const QString path = dir.filePath(transparent ? "init-change-u.ts" : "init-change-p.ts");
        ok &= expect(writeFile(path, pat + pmt0 + video + pmt0Again + video + pmt1 + video), label + ": write");
        M2tsPacketizer packetizer(path);
        packetizer.setTransparent(transparent);
        QString error;
        ok &= expect(packetizer.open(0, &error), label + ": opens");
        const QByteArray initial = packetizer.initData();
        M2tsObject object;
        QList<int> changedAt;
        QByteArray changed;
        for (int index = 0; packetizer.readObject(1, &object, &error); ++index) {
            QByteArray initData;
            if (packetizer.takeInitDataChange(&initData)) {
                changedAt.append(index);
                changed = initData;
            }
        }
        ok &= expect(changedAt == QList<int>({5}), label + ": one change, at the Object with the new PMT");
        ok &= expect(changed.mid(188) == pmt1 && changed.left(188) == initial.left(188),
                     label + ": the new initData has the same PAT and the new PMT");
    }

    // Live PSI tracking: the PMT moves to a new PID. The filter follows it, and
    // the rewritten PAT gets version 1 with the new PMT PID.
    {
        const QByteArray video = tb::tsPacket(0x100, false, QByteArray(184, char(0)));
        QByteArray ts = tb::psiPacket(0x0000, tb::patSection({{1, 0x1000}}, 0), 0);
        ts += tb::psiPacket(0x1000, tb::pmtSection(1, 0x100, {{0x1B, 0x100}})) + video;
        ts += tb::psiPacket(0x0000, tb::patSection({{1, 0x1100}}, 1), 1);
        ts += tb::psiPacket(0x1100, tb::pmtSection(1, 0x100, {{0x1B, 0x100}})) + video;
        const QString path = dir.filePath("pmt-moves.ts");
        ok &= expect(writeFile(path, ts), "write PMT move stream");
        const Run run = publish(path, false);
        ok &= expect(pids(run) == QList<int>({0x0000, 0x1000, 0x100, 0x0000, 0x1100, 0x100}),
                     "PMT move: the new PMT PID is kept");
        moq2ts::PsiAssembler assembler;
        QList<QByteArray> pats;
        for (const QByteArray& packet : run.packets) {
            if (pidOfPacket(packet) == 0) {
                for (const auto& section : assembler.push(packet, packet)) {
                    pats.append(section.bytes);
                }
            }
        }
        ok &= expect(pats.size() == 2 && pats.at(0) == tb::patSection({{1, 0x1000}}, 0) &&
                         pats.at(1) == tb::patSection({{1, 0x1100}}, 1),
                     "PMT move: rewritten PAT version 0, then 1 with the new PID");
    }

    // Live PSI tracking: the selected program leaves the PAT, which ends a
    // per-program track. Nothing after the change is published.
    {
        const QByteArray video = tb::tsPacket(0x100, false, QByteArray(184, char(0)));
        QByteArray ts = tb::psiPacket(0x0000, tb::patSection({{1, 0x1000}, {2, 0x1001}}, 0), 0);
        ts += tb::psiPacket(0x1000, tb::pmtSection(1, 0x100, {{0x1B, 0x100}})) + video;
        ts += tb::psiPacket(0x0000, tb::patSection({{2, 0x1001}}, 1), 1) + video;
        const QString path = dir.filePath("program-leaves.ts");
        ok &= expect(writeFile(path, ts), "write program-leaves stream");
        const Run run = publish(path, false, 1);
        ok &= expect(pids(run) == QList<int>({0x0000, 0x1000, 0x100}), "program leaves: nothing after the change");
        ok &= expect(run.error.contains("left the source PAT"), "program leaves: the track ends with a reason");
    }

    // A "next" PAT (current_next_indicator 0) without the program does not
    // apply yet, so the per-program track goes on.
    {
        const QByteArray video = tb::tsPacket(0x100, false, QByteArray(184, char(0)));
        QByteArray ts = tb::psiPacket(0x0000, tb::patSection({{1, 0x1000}, {2, 0x1001}}, 0), 0);
        ts += tb::psiPacket(0x1000, tb::pmtSection(1, 0x100, {{0x1B, 0x100}})) + video;
        ts += tb::psiPacket(0x0000, tb::patSection({{2, 0x1001}}, 1, 0, 0, false), 1) + video;
        const QString path = dir.filePath("next-pat.ts");
        ok &= expect(writeFile(path, ts), "write next-PAT stream");
        const Run run = publish(path, false, 1);
        ok &= expect(run.error.isEmpty() && pids(run).count(0x100) == 2, "next PAT: the track goes on");
    }

    // A PAT in two sections applies once both are in. Program 2, listed in the
    // second section only, opens and is not taken as absent.
    {
        const QByteArray video = tb::tsPacket(0x200, false, QByteArray(184, char(0)));
        QByteArray ts;
        for (int repeat = 0; repeat < 2; ++repeat) {
            ts += tb::psiPacket(0x0000, tb::patSection({{1, 0x1000}}, 0, 0, 1), 2 * repeat);
            ts += tb::psiPacket(0x0000, tb::patSection({{2, 0x1001}}, 0, 1, 1), 2 * repeat + 1);
            ts += tb::psiPacket(0x1001, tb::pmtSection(2, 0x200, {{0x1B, 0x200}}), repeat) + video;
        }
        const QString path = dir.filePath("two-section-pat.ts");
        ok &= expect(writeFile(path, ts), "write two-section PAT stream");
        const Run run = publish(path, false, 2);
        ok &= expect(run.error.isEmpty() && pids(run).count(0x200) == 2, "two-section PAT: program 2 runs");
        ok &= expect(pids(run).count(0x0000) == 2, "two-section PAT: one rewritten PAT per source PAT");
    }

    // The rewritten PAT keeps version 0 when another program moves its PMT.
    {
        const QByteArray video = tb::tsPacket(0x100, false, QByteArray(184, char(0)));
        QByteArray ts = tb::psiPacket(0x0000, tb::patSection({{1, 0x1000}, {2, 0x1001}}, 0), 0);
        ts += tb::psiPacket(0x1000, tb::pmtSection(1, 0x100, {{0x1B, 0x100}})) + video;
        ts += tb::psiPacket(0x0000, tb::patSection({{1, 0x1000}, {2, 0x1101}}, 1), 1) + video;
        const QString path = dir.filePath("other-program-moves.ts");
        ok &= expect(writeFile(path, ts), "write other-program stream");
        const Run run = publish(path, false, 1);
        moq2ts::PsiAssembler assembler;
        QList<QByteArray> pats;
        for (const QByteArray& packet : run.packets) {
            if (pidOfPacket(packet) == 0) {
                for (const auto& section : assembler.push(packet, packet)) {
                    pats.append(section.bytes);
                }
            }
        }
        ok &= expect(pats.size() == 2 && pats.at(0) == tb::patSection({{1, 0x1000}}, 0) && pats.at(1) == pats.at(0),
                     "rewritten PAT: version 0 when another program changes");
    }

    // Live PSI tracking: an unmodified-program source that becomes an MPTS ends
    // the track, because its mode is no longer true. A repeated PAT does not.
    {
        const QByteArray video = tb::tsPacket(0x100, false, QByteArray(184, char(0)));
        const QByteArray pmt = tb::psiPacket(0x1000, tb::pmtSection(1, 0x100, {{0x1B, 0x100}}));
        QByteArray ts = tb::psiPacket(0x0000, tb::patSection({{1, 0x1000}}, 0), 0) + pmt + video;
        ts += tb::psiPacket(0x0000, tb::patSection({{1, 0x1000}}, 0), 1) + video;
        ts += tb::psiPacket(0x0000, tb::patSection({{1, 0x1000}, {2, 0x1001}}, 1), 2) + video;
        const QString path = dir.filePath("spts-to-mpts.ts");
        ok &= expect(writeFile(path, ts), "write SPTS-to-MPTS stream");
        const Run run = publish(path, true);
        ok &= expect(run.packets.size() == 5, "SPTS to MPTS: the repeated PAT passes, the new one ends the track");
        ok &= expect(run.error.contains("unmodified-program track ends"), "SPTS to MPTS: the track ends with a reason");
    }

    // Random access (decision E2): a live source starting mid-GOP drops the
    // lead-in, so Group 0 starts at the random access point, and declares
    // mpeg2tsRandomAccess. A file keeps byte 0 and declares nothing. A live
    // multiplex keeps its lead-in and declares nothing.
    {
        const auto stream = [](const QList<std::pair<int, int>>& programs) {
            QByteArray ts = tb::psiPacket(0x0000, tb::patSection(programs));
            ts += tb::psiPacket(0x1000, tb::pmtSection(1, 0x100, {{0x1B, 0x100}}));
            ts += tb::tsPacket(0x100, false, QByteArray(184, char(0)), 0);            // mid-GOP
            ts += tb::tsPacket(0x100, true, tb::pesHeaderWithPts(9000), 1, 0x40);    // random access point
            ts += tb::tsPacket(0x100, false, QByteArray(182, char(0)), 2);
            return ts;
        };
        const QByteArray single = stream({{1, 0x1000}});
        const QByteArray rap = single.mid(3 * 188, 188);

        for (const bool transparent : {false, true}) {
            const std::string label = transparent ? "live unmodified-program" : "live per-program";
            const LiveRun live = publishLive(dir.filePath(transparent ? "live-u.fifo" : "live-p.fifo"), single, transparent);
            ok &= expect(live.run.error.isEmpty(), label + ": no error: " + live.run.error.toStdString());
            ok &= expect(live.randomAccess, label + ": declares random access");
            ok &= expect(!live.run.packets.isEmpty() && live.run.packets.first() == rap, label + ": starts at the RAP");
            ok &= expect(live.firstStartsGroup, label + ": Group 0 starts at the RAP");
            ok &= expect(live.initData == single.left(2 * 188) || !transparent,
                         label + ": initData holds the source PAT and PMT");
        }

        const QString file = dir.filePath("mid-gop.ts");
        ok &= expect(writeFile(file, single), "write mid-GOP file");
        M2tsPacketizer packetizer(file);
        QString error;
        ok &= expect(packetizer.open(0, &error) && !packetizer.randomAccess(), "file: no random access");
        M2tsObject object;
        ok &= expect(packetizer.readObject(1, &object, &error) && moq2ts::test::pidOfFirst(object.payload) == 0,
                     "file: byte 0 kept");

        // A live source whose encoder never sets random_access_indicator is
        // published from its first packet, without random access.
        QByteArray noIndicator = single;
        noIndicator[3 * 188 + 5] = static_cast<char>(static_cast<unsigned char>(noIndicator[3 * 188 + 5]) & ~0x40);
        for (const bool transparent : {false, true}) {
            const LiveRun live = publishLive(dir.filePath(transparent ? "no-rai-u.fifo" : "no-rai-p.fifo"), noIndicator, transparent);
            ok &= expect(!live.randomAccess && live.run.packets.size() == 5 && live.run.error.isEmpty(),
                         std::string(transparent ? "unmodified" : "per-program") +
                             " live source without indicator: published from its first packet, no random access");
        }

        // A live multiplex with a reference program (--program 1, whose PMT is
        // known) starts at that program's random access point, declares random
        // access, and carries the PAT and that program's PMT as initData.
        const QByteArray multiplex = stream({{1, 0x1000}, {2, 0x1001}});
        const LiveRun live = publishLive(dir.filePath("live-m.fifo"), multiplex, true, 1);
        ok &= expect(live.randomAccess && !live.run.packets.isEmpty() && live.run.packets.first() == rap &&
                         live.firstStartsGroup && live.initData == multiplex.left(2 * 188),
                     "live multiplex with --program: starts at the reference program's RAP, declares random access");
        // Without a requested program, it names no reference program: it keeps
        // its lead-in and declares nothing. Its initData is the PAT and the PMT
        // of the first program, so the missing PMT of program 2 does not matter.
        const LiveRun unnamed = publishLive(dir.filePath("live-m-unnamed.fifo"), multiplex, true);
        ok &= expect(!unnamed.randomAccess && unnamed.run.packets.size() == 5 &&
                         unnamed.initData == multiplex.left(2 * 188),
                     "live multiplex without --program: lead-in kept, no random access, initData of the first program");
        // Without a reference program (no PMT known), it keeps its lead-in and
        // declares nothing.
        QByteArray noReference = tb::psiPacket(0x0000, tb::patSection({{1, 0x1000}, {2, 0x1001}}));
        noReference += single.mid(2 * 188);
        const LiveRun none = publishLive(dir.filePath("live-m-none.fifo"), noReference, true);
        ok &= expect(!none.randomAccess && none.run.packets.size() == 4,
                     "live multiplex without a reference program: lead-in kept, no random access");
    }

    // Group boundaries follow the video PID's random_access_indicator, also
    // when the PCR has a PID of its own, in both modes, for a file and a live
    // source.
    {
        QByteArray ts = tb::psiPacket(0x0000, tb::patSection({{1, 0x1000}}));
        ts += tb::psiPacket(0x1000, tb::pmtSection(1, 0x1FF, {{0x1B, 0x100}}));
        for (int gop = 0; gop < 3; ++gop) {
            ts += tb::tsPacket(0x100, true, tb::pesHeaderWithPts(9000 * gop), gop * 2, 0x40);
            ts += tb::tsPacket(0x100, false, QByteArray(184, char(0)), gop * 2 + 1);
        }
        const QString path = dir.filePath("separate-pcr.ts");
        ok &= expect(writeFile(path, ts), "write separate-PCR stream");
        for (const bool transparent : {false, true}) {
            const std::string label = transparent ? "separate PCR, unmodified" : "separate PCR, per-program";
            M2tsPacketizer packetizer(path);
            packetizer.setTransparent(transparent);
            QString error;
            ok &= expect(packetizer.open(0, &error), label + ": opens");
            M2tsObject object;
            int groups = 0;
            while (packetizer.readObject(1, &object, &error)) {
                groups += object.startsGroup ? 1 : 0;
            }
            ok &= expect(groups == 3, label + ": one Group per random access point");
            const LiveRun live = publishLive(dir.filePath(transparent ? "sep-pcr-u.fifo" : "sep-pcr-p.fifo"), ts, transparent);
            ok &= expect(live.randomAccess && live.firstStartsGroup, label + ", live: random access declared");
        }
    }

    // Conditional access: a scrambled MPTS with one CA system per program. The
    // per-program track keeps its ECMs (from the PMT), the CAT, and the EMMs of
    // its CA system (from the CAT). The CAT lists only that system.
    {
        const QByteArray pat = tb::psiPacket(0x0000, tb::patSection({{1, 0x1000}, {2, 0x1001}}));
        const QByteArray pmt1 = tb::psiPacket(0x1000, tb::pmtSectionWithDescriptors(
            1, 0x100, tb::caDescriptor(0x0B00, 0x600), {{0x1B, 0x100, {}}, {0x0F, 0x101, tb::caDescriptor(0x0B00, 0x602)}}));
        const QByteArray pmt2 = tb::psiPacket(0x1001, tb::pmtSectionWithDescriptors(
            2, 0x200, tb::caDescriptor(0x0500, 0x601), {{0x1B, 0x200, {}}}));
        const QByteArray catBytes = tb::catSection(tb::caDescriptor(0x0B00, 0x700) + tb::caDescriptor(0x0500, 0x701));
        // A later CAT version adds a second EMM stream for the program's CA system.
        const QByteArray catChanged = tb::catSection(tb::caDescriptor(0x0B00, 0x700) + tb::caDescriptor(0x0500, 0x701) +
                                                     tb::caDescriptor(0x0B00, 0x703), 1);
        const auto data = [](int pid, int cc) { return tb::tsPacket(pid, false, QByteArray(184, char(0)), cc); };
        QByteArray ts = pat + pmt1 + pmt2;
        for (int repeat = 0; repeat < 3; ++repeat) {
            ts += tb::psiPacket(0x0001, repeat < 2 ? catBytes : catChanged, repeat);
            for (int pid : {0x600, 0x601, 0x602, 0x700, 0x701, 0x703, 0x100, 0x200}) {
                ts += data(pid, repeat);
            }
        }
        const QString path = dir.filePath("scrambled.ts");
        ok &= expect(writeFile(path, ts), "write scrambled MPTS");
        const Run run = publish(path, false, 1);
        const QList<int> out = pids(run);
        ok &= expect(out.count(0x600) == 3 && out.count(0x602) == 3, "CA: program and ES ECMs kept");
        ok &= expect(out.count(0x700) == 3, "CA: EMM of the program's CA system kept");
        ok &= expect(out.count(0x703) == 1, "CA: an EMM PID that a new CAT adds is kept from then on");
        ok &= expect(!out.contains(0x601) && !out.contains(0x701) && !out.contains(0x200), "CA: other program's CA PIDs dropped");
        moq2ts::PsiAssembler assembler;
        QList<QByteArray> cats;
        for (const QByteArray& packet : run.packets) {
            if (pidOfPacket(packet) == 0x0001) {
                for (const auto& section : assembler.push(packet, packet)) {
                    cats.append(section.bytes);
                }
            }
        }
        ok &= expect(cats.size() == 3 && cats.at(0) == tb::catSection(tb::caDescriptor(0x0B00, 0x700)) && cats.at(1) == cats.at(0),
                     "CA: CAT lists only the program's CA system, version 0 on repeat, valid CRC_32");
        ok &= expect(cats.value(2) == tb::catSection(tb::caDescriptor(0x0B00, 0x700) + tb::caDescriptor(0x0B00, 0x703), 1),
                     "CA: the changed CAT moves to version 1");

        // A clear program of the same multiplex gets no CAT and no CA PIDs.
        QByteArray clear = tb::psiPacket(0x0000, tb::patSection({{1, 0x1000}, {3, 0x1003}}));
        clear += tb::psiPacket(0x1003, tb::pmtSection(3, 0x300, {{0x1B, 0x300}}));
        clear += tb::psiPacket(0x0001, catBytes) + data(0x700, 0) + data(0x300, 0);
        const QString clearPath = dir.filePath("clear-in-scrambled.ts");
        ok &= expect(writeFile(clearPath, clear), "write clear-program stream");
        ok &= expect(pids(publish(clearPath, false, 3)) == QList<int>({0x0000, 0x1003, 0x300}),
                     "CA: a clear program gets no CAT and no EMM");
    }

    // SI rewrite with --retain-si: the SDT actual keeps the carried service in
    // one section; SDT other and EIT other go; EIT actual keeps the carried
    // service's sections unchanged; the BAT and the TDT pass unchanged.
    {
        const QByteArray name1("\x48\x03\x01\x00\x00", 5);   // a short service_descriptor
        const QByteArray sdtActual0 = tb::longSection(0x42, 1, tb::sdtBody(0x22, {{2, {}}, {3, {}}}), 4, 0, 1);
        const QByteArray sdtActual1 = tb::longSection(0x42, 1, tb::sdtBody(0x22, {{1, name1}}), 4, 1, 1);
        const QByteArray sdtOther = tb::longSection(0x46, 9, tb::sdtBody(0x22, {{1, {}}}));
        const QByteArray bat = tb::longSection(0x4A, 0x1234, QByteArray("\xF0\x00\xF0\x00", 4));
        const QByteArray eitService1 = tb::longSection(0x4E, 1, QByteArray(6, char(0)));
        const QByteArray eitService2 = tb::longSection(0x4E, 2, QByteArray(6, char(0)));
        const QByteArray eitSchedule1 = tb::longSection(0x50, 1, QByteArray(6, char(0)));
        const QByteArray eitOther = tb::longSection(0x4F, 1, QByteArray(6, char(0)));
        QByteArray tdt("\x70\x70\x05\xE0\x00\x12\x00\x00", 8);   // section without CRC_32
        QByteArray ts = tb::psiPacket(0x0000, tb::patSection({{1, 0x1000}, {2, 0x1001}, {3, 0x1002}}));
        ts += tb::psiPacket(0x1000, tb::pmtSection(1, 0x100, {{0x1B, 0x100}}));
        for (int repeat = 0; repeat < 2; ++repeat) {
            int sdtCc = repeat * 4;
            for (const QByteArray& section : {sdtActual0, sdtActual1, sdtOther, bat}) {
                ts += tb::psiPacket(0x0011, section, sdtCc++);
            }
            int eitCc = repeat * 4;
            for (const QByteArray& section : {eitService1, eitService2, eitSchedule1, eitOther}) {
                ts += tb::psiPacket(0x0012, section, eitCc++);
            }
            ts += tb::psiPacket(0x0014, tdt, repeat);
        }
        const QString path = dir.filePath("si.ts");
        ok &= expect(writeFile(path, ts), "write SI stream");

        const Run run = publish(path, false, 1, true);
        QMap<int, QList<QByteArray>> tables;
        QMap<int, moq2ts::PsiAssembler> assemblers;
        for (const QByteArray& packet : run.packets) {
            const int pid = pidOfPacket(packet);
            if (pid == 0x0011 || pid == 0x0012) {
                for (const auto& section : assemblers[pid].push(packet, packet)) {
                    tables[pid].append(section.bytes);
                }
            }
        }
        const QByteArray sdtExpected = tb::longSection(0x42, 1, tb::sdtBody(0x22, {{1, name1}}), 0, 0, 0);
        ok &= expect(tables.value(0x0011) == QList<QByteArray>({sdtExpected, bat, sdtExpected, bat}),
                     "SI: SDT actual reduced to service 1, version 0 on repeat; SDT other dropped; BAT unchanged");
        ok &= expect(tables.value(0x0012) == QList<QByteArray>({eitService1, eitSchedule1, eitService1, eitSchedule1}),
                     "SI: EIT actual of service 1 kept unchanged; other services and EIT other dropped");
        QList<QByteArray> tdtOut;
        for (const QByteArray& packet : run.packets) {
            if (pidOfPacket(packet) == 0x0014) {
                tdtOut.append(packet);
            }
        }
        ok &= expect(tdtOut.size() == 2 && tdtOut.at(0) == ts.mid(ts.indexOf(tb::psiPacket(0x0014, tdt, 0)), 188),
                     "SI: TDT passes unchanged");
        ok &= expect(!pids(publish(path, false, 1, false)).contains(0x0011), "SI: without --retain-si, no SI is kept");
    }

    // SI table selection: only the chosen tables stay. SDT and BAT share PID
    // 0x0011, and EIT present/following and schedule share PID 0x0012; the PID
    // is retained when either is chosen and the other's sections go.
    {
        const QByteArray sdtActual = tb::longSection(0x42, 1, tb::sdtBody(0x22, {{1, {}}}));
        const QByteArray bat = tb::longSection(0x4A, 0x1234, QByteArray("\xF0\x00\xF0\x00", 4));
        const QByteArray eitPf = tb::longSection(0x4E, 1, QByteArray(6, char(0)));
        const QByteArray eitSchedule = tb::longSection(0x50, 1, QByteArray(6, char(0)));
        QByteArray ts = tb::psiPacket(0x0000, tb::patSection({{1, 0x1000}}));
        ts += tb::psiPacket(0x1000, tb::pmtSection(1, 0x100, {{0x1B, 0x100}}));
        int sdtCc = 0;
        for (const QByteArray& section : {sdtActual, bat}) {
            ts += tb::psiPacket(0x0011, section, sdtCc++);
        }
        int eitCc = 0;
        for (const QByteArray& section : {eitPf, eitSchedule}) {
            ts += tb::psiPacket(0x0012, section, eitCc++);
        }
        ts += tb::psiPacket(0x0014, QByteArray("\x70\x70\x05\xE0\x00\x12\x00\x00", 8), 0);
        const QString path = dir.filePath("si-select.ts");
        ok &= expect(writeFile(path, ts), "write SI selection stream");

        // The sections that a PID carries after the selection, and the PIDs it advertises.
        const auto selected = [&](const QSet<moq2ts::SiTable>& tables, QList<int>* siPids, int pid) {
            M2tsPacketizer packetizer(path);
            packetizer.setSiTables(tables);
            QString error;
            QList<QByteArray> sections;
            if (!packetizer.open(1, &error)) {
                return sections;
            }
            if (siPids != nullptr) {
                *siPids = packetizer.retainedSiPids();
            }
            moq2ts::PsiAssembler assembler;
            M2tsObject object;
            while (packetizer.readObject(1, &object, &error)) {
                if (pidOfPacket(object.payload) == pid) {
                    for (const auto& section : assembler.push(object.payload, object.payload)) {
                        sections.append(section.bytes);
                    }
                }
            }
            return sections;
        };
        using moq2ts::SiTable;
        QList<int> siPids;
        const QSet<SiTable> sdtOnly{SiTable::Sdt};
        ok &= expect(!selected(sdtOnly, &siPids, 0x0011).isEmpty() && selected(sdtOnly, nullptr, 0x0012).isEmpty() &&
                         selected(sdtOnly, nullptr, 0x0014).isEmpty() && siPids == QList<int>({0x0011}),
                     "SI select: sdt only keeps PID 0x0011, drops 0x0012 and 0x0014, retains {0x0011}");
        ok &= expect(selected(sdtOnly, nullptr, 0x0011).size() == 1 && selected(sdtOnly, nullptr, 0x0011).first()[0] == char(0x42),
                     "SI select: sdt without bat drops the BAT section on PID 0x0011");
        const QSet<SiTable> batOnly{SiTable::Bat};
        ok &= expect(selected(batOnly, &siPids, 0x0011) == QList<QByteArray>({bat}) && siPids == QList<int>({0x0011}),
                     "SI select: bat without sdt keeps only the BAT section");
        const QSet<SiTable> eitPfOnly{SiTable::EitPresentFollowing};
        ok &= expect(selected(eitPfOnly, &siPids, 0x0012) == QList<QByteArray>({eitPf}) && siPids == QList<int>({0x0012}) &&
                         selected(eitPfOnly, nullptr, 0x0011).isEmpty(),
                     "SI select: eit-pf only keeps p/f sections, drops schedule, retains {0x0012}");
        const QSet<SiTable> eitScheduleOnly{SiTable::EitSchedule};
        ok &= expect(selected(eitScheduleOnly, nullptr, 0x0012) == QList<QByteArray>({eitSchedule}),
                     "SI select: eit-schedule only keeps schedule sections");
        ok &= expect(selected(moq2ts::allSiTables(), &siPids, 0x0011) == QList<QByteArray>({selected(sdtOnly, nullptr, 0x0011).first(), bat}) &&
                         siPids == QList<int>({0x0010, 0x0011, 0x0012, 0x0014}),
                     "SI select: all tables keeps the retain-si output and PIDs");

        QSet<SiTable> parsed;
        QString parseError;
        ok &= expect(moq2ts::parseSiTables(QStringLiteral("sdt, eit-pf,tdt-tot"), &parsed, &parseError) &&
                         parsed == QSet<SiTable>({SiTable::Sdt, SiTable::EitPresentFollowing, SiTable::TdtTot}),
                     "SI parse: comma-separated names");
        ok &= expect(moq2ts::parseSiTables(QStringLiteral("all"), &parsed, &parseError) && parsed == moq2ts::allSiTables(),
                     "SI parse: all");
        ok &= expect(!moq2ts::parseSiTables(QStringLiteral("sdt,foo"), &parsed, &parseError) && parseError.contains("foo"),
                     "SI parse: an unknown name fails and the error names it");
    }

    // Mux rate: a CBR SPTS at 3,008,000 bit/s (2,000 packets per second) with
    // a PCR every 100 packets gives exactly that rate. One PCR, no null
    // packets, or an MPTS give no value.
    {
        const auto stream = [](const QList<std::pair<int, int>>& programs, int pcrEvery, int nullEvery, int packets) {
            QByteArray ts = tb::psiPacket(0x0000, tb::patSection(programs));
            ts += tb::psiPacket(0x1000, tb::pmtSection(1, 0x100, {{0x1B, 0x100}}));
            int videoCc = 0;
            for (int index = 2; index < packets; ++index) {
                if (index % pcrEvery == 0) {
                    ts += pcrPacket(0x100, static_cast<std::int64_t>(index) * 13500, videoCc++);   // 27e6 / 2000
                } else if (nullEvery > 0 && index % nullEvery == 0) {
                    ts += tb::tsPacket(0x1FFF, false, QByteArray(184, char(0xFF)));
                } else {
                    ts += tb::tsPacket(0x100, false, QByteArray(184, char(0)), videoCc++);
                }
            }
            return ts;
        };
        const auto measure = [&](const QString& name, const QByteArray& ts, QString* note) {
            const QString path = dir.filePath(name);
            writeFile(path, ts);
            M2tsPacketizer packetizer(path);
            QString error;
            packetizer.open(0, &error);
            *note = packetizer.muxRateNote();
            return packetizer.measuredMuxRate();
        };
        QString note;
        ok &= expect(measure("cbr.ts", stream({{1, 0x1000}}, 100, 3, 3000), &note) == 3008000 && note.isEmpty(),
                     "mux rate: CBR SPTS measured exactly");
        ok &= expect(measure("one-pcr.ts", stream({{1, 0x1000}}, 5000, 3, 3000), &note) == 0 && note.contains("two PCRs"),
                     "mux rate: one PCR gives no value");
        ok &= expect(measure("vbr.ts", stream({{1, 0x1000}}, 100, 0, 3000), &note) == 0 && note.contains("VBR"),
                     "mux rate: no null packets gives no value");
        ok &= expect(measure("mpts-rate.ts", stream({{1, 0x1000}, {2, 0x1001}}, 100, 3, 3000), &note) == 0,
                     "mux rate: not measured for an MPTS");

        // The MSF bitrate: the packets the track carries, in every mode. An
        // unmodified track carries every packet, so CBR or VBR, one program or
        // several, it is the source rate. A per-program track without its null
        // packets carries less.
        const auto bitrate = [&](const QString& name, const QByteArray& ts, bool transparent) {
            const QString path = dir.filePath(name);
            writeFile(path, ts);
            M2tsPacketizer packetizer(path);
            packetizer.setTransparent(transparent);
            QString error;
            packetizer.open(0, &error);
            return packetizer.measuredBitrate();
        };
        ok &= expect(bitrate("rate-u.ts", stream({{1, 0x1000}}, 100, 3, 3000), true) == 3008000,
                     "bitrate: unmodified-program is the source rate");
        ok &= expect(bitrate("rate-m.ts", stream({{1, 0x1000}, {2, 0x1001}}, 100, 3, 3000), true) == 3008000,
                     "bitrate: unmodified-multiplex is the source rate");
        ok &= expect(bitrate("rate-vbr.ts", stream({{1, 0x1000}}, 100, 0, 3000), true) == 3008000,
                     "bitrate: measured without null packets too");
        const qint64 perProgram = bitrate("rate-p.ts", stream({{1, 0x1000}}, 100, 3, 3000), false);
        ok &= expect(perProgram > 1900000 && perProgram < 2100000,
                     "bitrate: per-program without null packets counts the packets it keeps (" +
                         std::to_string(perProgram) + ")");
        ok &= expect(bitrate("rate-one-pcr.ts", stream({{1, 0x1000}}, 5000, 3, 3000), true) == 0,
                     "bitrate: one PCR gives no value");
        // The measure is the maximum (MSF bitrate) only where the rate is
        // constant: an unmodified track of a source with null stuffing.
        const auto constant = [&](const QString& name, const QByteArray& ts, bool transparent) {
            const QString path = dir.filePath(name);
            writeFile(path, ts);
            M2tsPacketizer packetizer(path);
            packetizer.setTransparent(transparent);
            QString error;
            packetizer.open(0, &error);
            return packetizer.measuredBitrateIsConstant();
        };
        ok &= expect(constant("const-u.ts", stream({{1, 0x1000}}, 100, 3, 3000), true),
                     "bitrate: constant for an unmodified track of a stuffed source");
        ok &= expect(!constant("const-vbr.ts", stream({{1, 0x1000}}, 100, 0, 3000), true),
                     "bitrate: not constant without null packets");
        ok &= expect(!constant("const-p.ts", stream({{1, 0x1000}}, 100, 3, 3000), false),
                     "bitrate: not constant for a per-program track, which drops the null packets");

        // A per-program track of a 2-program stream measures its own program's
        // packets, not the multiplex. Packets 0..2 are the PAT and the two PMTs;
        // from packet 3, every 4th packet is program 1 (PID 0x100, which also
        // carries the PCR every 100 packets) and the others are program 2
        // (PID 0x200). At 2,000 packets/s (13,500 ticks each), the window
        // from the PCR at packet 100 to the one at packet 2100 is exactly 1 s
        // and holds 2,000 packets, 500 of them program 1: 500 * 188 * 8 =
        // 752,000 bit/s, against 2,000 * 188 * 8 = 3,008,000 bit/s for the
        // multiplex.
        {
            QByteArray ts = tb::psiPacket(0x0000, tb::patSection({{1, 0x1000}, {2, 0x1001}}));
            ts += tb::psiPacket(0x1000, tb::pmtSection(1, 0x100, {{0x1B, 0x100}}));
            ts += tb::psiPacket(0x1001, tb::pmtSection(2, 0x200, {{0x1B, 0x200}}));
            int cc1 = 0;
            int cc2 = 0;
            for (int index = 3; index < 3000; ++index) {
                if (index % 100 == 0) {
                    ts += pcrPacket(0x100, static_cast<std::int64_t>(index) * 13500, cc1++);
                } else if (index % 4 == 0) {
                    ts += tb::tsPacket(0x100, false, QByteArray(184, char(0)), cc1++);
                } else {
                    ts += tb::tsPacket(0x200, false, QByteArray(184, char(0)), cc2++);
                }
            }
            const QString path = dir.filePath("rate-2prog.ts");
            writeFile(path, ts);
            const auto programRate = [&](int program) {
                M2tsPacketizer packetizer(path);
                QString error;
                packetizer.open(program, &error);
                return packetizer.measuredBitrate();
            };
            ok &= expect(programRate(1) == 752000,
                         "bitrate: per-program track of a 2-program stream is the program's rate, not the multiplex rate (" +
                             std::to_string(programRate(1)) + ")");
        }
    }

    // Source gaps (SRT ingest): no PCR discontinuity within a Group (draft "PCR
    // and Timing"). A gap that comes after the first Object ends the Object
    // being filled and starts a Group. One known before the first Object drops
    // what came before it, so the track starts at the live edge.
    {
        QByteArray ts = tb::psiPacket(0x0000, tb::patSection({{1, 0x1000}}));            // 0
        ts += tb::psiPacket(0x1000, tb::pmtSection(1, 0x100, {{0x1B, 0x100}}));          // 1
        ts += tb::tsPacket(0x100, true, tb::pesHeaderWithPts(9000), 0, 0x40);           // 2: RAP
        ts += tb::tsPacket(0x100, false, QByteArray(184, char(0)), 1);                   // 3
        ts += tb::tsPacket(0x100, false, QByteArray(184, char(0)), 2);                   // 4
        ts += tb::tsPacket(0x100, false, QByteArray(184, char(0)), 3);                   // 5
        ts += tb::tsPacket(0x100, true, tb::pesHeaderWithPts(900000), 9, 0x40);         // 6: RAP after the gap
        ts += tb::tsPacket(0x100, false, QByteArray(184, char(0)), 10);                  // 7
        ts += tb::tsPacket(0x100, false, QByteArray(184, char(0)), 11);                  // 8
        const auto packet = [&ts](int index) { return ts.mid(index * 188, 188); };
        constexpr std::uint64_t gapOffset = 6 * 188;

        struct GapRun {
            QList<M2tsObject> objects;
            QString error;
            bool randomAccess = false;
        };
        // gapBeforeFirst: the gap is known at open(); otherwise it is recorded
        // once the first Object is out. A queue overflow at laterOffset, when
        // given, is recorded with it.
        const auto run = [&](const QString& name, bool gapBeforeFirst, std::uint64_t offset,
                             unsigned cause = SourceGapLog::SrtReceiveBuffer, std::uint64_t laterOffset = 0) {
            GapRun result;
            const QString fifo = dir.filePath(name);
            ::mkfifo(fifo.toLocal8Bit().constData(), 0600);
            std::thread writer([&] { writeFile(fifo, ts); });
            auto gaps = std::make_shared<moq2ts::SourceGapLog>();
            M2tsPacketizer packetizer(fifo);
            packetizer.setTransparent(true);
            packetizer.setSourceGaps(gaps);
            if (gapBeforeFirst) {
                gaps->record(offset, cause);
                gaps->record(laterOffset, SourceGapLog::ProgramQueue);
            }
            if (packetizer.open(0, &result.error)) {
                result.randomAccess = packetizer.randomAccess();
                M2tsObject object;
                while (packetizer.readObject(3, &object, &result.error)) {
                    result.objects.append(object);
                    if (!gapBeforeFirst && result.objects.size() == 1) {
                        gaps->record(offset, cause);
                        gaps->record(laterOffset, SourceGapLog::ProgramQueue);
                    }
                }
            }
            writer.join();
            return result;
        };

        const GapRun mid = run("gap-mid.fifo", false, gapOffset);
        ok &= expect(!mid.randomAccess, "gap-capable live track does not promise random access");
        ok &= expect(mid.error.isEmpty() && mid.objects.size() == 3, "gap after the first Object: three Objects");
        if (mid.objects.size() == 3) {
            ok &= expect(mid.objects[0].payload == packet(2) + packet(3) + packet(4) && mid.objects[0].groupId == 0,
                         "gap after the first Object: Group 0 starts at the first RAP");
            ok &= expect(mid.objects[1].payload == packet(5) && mid.objects[1].groupId == 0 &&
                             !mid.objects[1].startsGroup,
                         "gap after the first Object: the Object before the gap ends at it");
            ok &= expect(mid.objects[2].payload == packet(6) + packet(7) + packet(8) &&
                             mid.objects[2].groupId == 1 && mid.objects[2].objectId == 0 &&
                             mid.objects[2].startsGroup,
                         "gap after the first Object: Group 1 starts where the source resumed");
        }

        const GapRun first = run("gap-first.fifo", true, gapOffset);
        ok &= expect(first.error.isEmpty() && first.objects.size() == 1 &&
                         first.objects[0].payload == packet(6) + packet(7) + packet(8) &&
                         first.objects[0].groupId == 0 && first.objects[0].startsGroup,
                     "gap before the first Object: the track starts where the source resumed, in Group 0");

        // The warning names the hop that skipped ahead.
        qInstallMessageHandler(captureWarning);
        warnings().clear();
        run("gap-srt.fifo", false, gapOffset);
        const QStringList srtWarnings = warnings();
        warnings().clear();
        run("gap-queue.fifo", false, gapOffset, SourceGapLog::ProgramQueue);
        const QStringList queueWarnings = warnings();
        warnings().clear();
        run("gap-caller.fifo", false, gapOffset, SourceGapLog::SrtNewCaller);
        const QStringList callerWarnings = warnings();
        warnings().clear();
        run("gap-first-named.fifo", true, gapOffset);
        const QStringList firstWarnings = warnings();
        qInstallMessageHandler(nullptr);
        // Several programs share one log: the warning names the track (the
        // packetizer's label, here its source path), as the start-up lines do.
        ok &= expect(srtWarnings.filter("skipped ahead").size() == 1 &&
                         srtWarnings.filter("skipped ahead").front().startsWith(dir.filePath("gap-srt.fifo") + ": "),
                     "gap warning: names the track: " + srtWarnings.join(" | ").toStdString());
        ok &= expect(firstWarnings.filter("skipped ahead").size() == 1 &&
                         firstWarnings.filter("skipped ahead").front().startsWith(dir.filePath("gap-first-named.fifo") +
                                                                                 ": "),
                     "gap warning before the first Object: names the track: " +
                         firstWarnings.join(" | ").toStdString());
        ok &= expect(callerWarnings.filter("new SRT caller").size() == 1 &&
                         callerWarnings.filter("receive buffer").isEmpty(),
                     "gap warning: a new SRT caller is named, not the receive buffer: " +
                         callerWarnings.join(" | ").toStdString());
        ok &= expect(srtWarnings.filter("SRT receive buffer").size() == 1,
                     "gap warning: an SRT gap names the SRT receive buffer");
        ok &= expect(queueWarnings.filter("queue").size() == 1 && queueWarnings.filter("SRT").isEmpty(),
                     "gap warning: a queue overflow names the program's queue, not SRT: " +
                         queueWarnings.join(" | ").toStdString());

        // The live-edge flush at the first pull is the normal start of a track.
        // An overflow at its offset only dropped what the flush drops anyway,
        // so neither the gap before the first Object nor a later one warns. An
        // overflow at another offset still warns, and names only the queue.
        constexpr unsigned edgeAndQueue = SourceGapLog::LiveEdge | SourceGapLog::ProgramQueue;
        for (const bool gapBeforeFirst : {true, false}) {
            const std::string where = gapBeforeFirst ? "before the first Object" : "after the first Object";
            warnings().clear();
            qInstallMessageHandler(captureWarning);
            run(gapBeforeFirst ? "gap-edge-first.fifo" : "gap-edge-mid.fifo", gapBeforeFirst, gapOffset, edgeAndQueue);
            const QStringList edgeWarnings = warnings();
            warnings().clear();
            run(gapBeforeFirst ? "gap-edge-later-first.fifo" : "gap-edge-later-mid.fifo", gapBeforeFirst, gapOffset,
                edgeAndQueue, 8 * 188);
            const QStringList laterWarnings = warnings();
            qInstallMessageHandler(nullptr);
            ok &= expect(edgeWarnings.filter("skipped ahead").isEmpty(),
                         "live edge with an overflow at its offset, " + where + ": no warning: " +
                             edgeWarnings.join(" | ").toStdString());
            ok &= expect(laterWarnings.filter("skipped ahead").size() == 1 &&
                             laterWarnings.filter("queue overflowed").size() == 1 &&
                             laterWarnings.filter("first pull").isEmpty(),
                         "live edge, then an overflow at another offset, " + where +
                             ": one warning, for the queue: " + laterWarnings.join(" | ").toStdString());
        }

        const GapRun none = run("gap-zero.fifo", true, 0);
        ok &= expect(none.error.isEmpty() && none.objects.size() == 3 &&
                         none.objects[0].payload == packet(2) + packet(3) + packet(4) &&
                         none.objects[1].payload == packet(5) + packet(6) + packet(7) &&
                         none.objects[1].groupId == 1 && none.objects[2].groupId == 1,
                     "gap at offset 0: no gap, the Groups follow the RAPs");
    }

    // A PMT whose PCR_PID is 0 (the PAT PID) names no PCR PID: PIDs 0x0000 to
    // 0x000F carry tables, never a PCR. After a gap, per-program carriage
    // looks for the first PCR to mark; the PAT packets it rewrites are not
    // looked at, and go out with their table unchanged.
    {
        const auto video = [](int cc, bool rap) {
            return rap ? tb::tsPacket(0x100, true, tb::pesHeaderWithPts(9000 * cc), cc, 0x40)
                       : tb::tsPacket(0x100, false, QByteArray(184, char(cc)), cc);
        };
        QByteArray ts = tb::psiPacket(0x0000, tb::patSection({{1, 0x1000}}), 0);
        ts += tb::psiPacket(0x1000, tb::pmtSection(1, 0x0000, {{0x1B, 0x100}}));
        ts += video(0, true) + video(1, false) + tb::psiPacket(0x0000, tb::patSection({{1, 0x1000}}), 1);
        const std::uint64_t gapOffset = static_cast<std::uint64_t>(ts.size());
        ts += video(2, true) + tb::psiPacket(0x0000, tb::patSection({{1, 0x1000}}), 2) + video(3, false);
        const QString fifo = dir.filePath("pcr-pid-zero.fifo");
        ::mkfifo(fifo.toLocal8Bit().constData(), 0600);
        std::thread writer([&] { writeFile(fifo, ts); });
        auto gaps = std::make_shared<moq2ts::SourceGapLog>();
        M2tsPacketizer packetizer(fifo);
        packetizer.setSourceGaps(gaps);
        QString error;
        QList<QByteArray> patPackets;
        bool opened = packetizer.open(1, &error);
        if (opened) {
            M2tsObject object;
            while (packetizer.readObject(1, &object, &error)) {
                if (object.objectId == 0 && object.groupId == 0) {
                    gaps->record(gapOffset);
                }
                if (pidOfPacket(object.payload) == 0x0000) {
                    patPackets.append(object.payload);
                }
            }
        }
        writer.join();
        ok &= expect(opened, "PCR_PID 0: opens: " + error.toStdString());
        ok &= expect(packetizer.pcrPid() == -1, "PCR_PID 0: no PCR PID (" + std::to_string(packetizer.pcrPid()) + ")");
        bool unchanged = !patPackets.isEmpty();
        for (const QByteArray& packet : patPackets) {
            unchanged = unchanged && packet.size() == 188 && packet[4] == char(0x00) && packet[5] == char(0x00);
        }
        ok &= expect(unchanged, "PCR_PID 0: the rewritten PAT packets keep pointer_field 0 and table_id 0");
    }

    // Timed start-up windows on a live source keep no media: a program whose
    // PCR PID carries no PCR ends its start-up promptly, with an error that
    // says so, after reading a bounded number of packets and keeping none.
    {
        QByteArray ts = tb::psiPacket(0x0000, tb::patSection({{1, 0x1000}}));
        ts += tb::psiPacket(0x1000, tb::pmtSection(1, 0x100, {{0x1B, 0x100}}));
        const QByteArray video = tb::tsPacket(0x100, false, QByteArray(184, char(0)));
        QByteArray body;
        for (int i = 0; i < 1000; ++i) {
            body += video;
        }
        const QString fifo = dir.filePath("no-pcr.fifo");
        ::mkfifo(fifo.toLocal8Bit().constData(), 0600);
        std::atomic<bool> done{false};
        std::thread writer([&] {
            const int fd = ::open(fifo.toLocal8Bit().constData(), O_WRONLY);
            if (fd < 0) {
                return;
            }
            bool open = ::write(fd, ts.constData(), ts.size()) == ts.size();
            // Up to 400,000 packets, more than any window reads.
            for (int i = 0; open && !done && i < 400; ++i) {
                open = ::write(fd, body.constData(), body.size()) == body.size();
            }
            ::close(fd);
        });
        auto packetizer = std::make_unique<M2tsPacketizer>(fifo);
        packetizer->setTimedStartWindows(true);
        QString error;
        const auto begin = std::chrono::steady_clock::now();
        const bool opened = packetizer->open(1, &error);
        const auto took = std::chrono::steady_clock::now() - begin;
        const qsizetype kept = packetizer->prebufferedBytes();
        done = true;
        packetizer.reset();   // closes the FIFO, which ends the writer
        writer.join();
        ok &= expect(!opened && error.contains(QLatin1String("PCR")),
                     "timed windows, no PCR: start-up fails and says why: " + error.toStdString());
        ok &= expect(took < std::chrono::seconds(10), "timed windows, no PCR: start-up ends promptly");
        ok &= expect(kept == 0, "timed windows: the scans keep no packet (" + std::to_string(kept) + " bytes)");
    }

    // Timed windows across PCR discontinuities, as a looping feed or an
    // encoder restart gives: the PCR steps back at the start, before the PSI,
    // jumps ahead in the rate window (1 s, measured again from the jump at
    // 1 s, so it ends at 2 s), and jumps ahead and back before the first
    // random access point. A step back, or one over 1 s, counts as no time,
    // so the windows still find the PSI (at 0.4 s) and the random access
    // point (at 4 s).
    {
        const auto pcrAt = [](double t) {
            const double clock = t < 1.0 ? t : t < 3.0 ? t + 100 : t < 3.5 ? t + 200 : t + 150;
            return static_cast<std::int64_t>(clock * 27e6);
        };
        QByteArray ts;
        int cc = 0;
        for (int k = 0; k < 3; ++k) {
            ts += pcrPacket(0x100, static_cast<std::int64_t>((25.0 + 0.04 * k) * 27e6), cc++);   // before the loop
        }
        for (int slot = 0; slot < 125; ++slot) {
            const double t = slot * 0.04;
            if (slot == 10) {
                ts += tb::psiPacket(0x0000, tb::patSection({{1, 0x1000}}));
                ts += tb::psiPacket(0x1000, tb::pmtSection(1, 0x100, {{0x1B, 0x100}}));
            }
            ts += pcrPacket(0x100, pcrAt(t), cc++);
            if (slot == 100) {
                ts += tb::tsPacket(0x100, true, tb::pesHeaderWithPts(360000), cc++, 0x40);
            }
            ts += tb::tsPacket(0x100, false, QByteArray(184, char(0)), cc++);
        }
        bool randomAccess = false;
        QString error;
        const bool opened = timedOpen(dir.filePath("pcr-jumps.fifo"), ts, 1, &randomAccess, &error);
        ok &= expect(opened, "timed windows, PCR jumps: the PSI window finds the PAT and PMT: " + error.toStdString());
        ok &= expect(randomAccess, "timed windows, PCR jumps: the random access window finds the RAP");
    }

    // A program whose PMT names no PCR PID (0x1FFF, which ISO/IEC 13818-1
    // allows) times its windows on the PCR of another program of the
    // multiplex, and starts.
    {
        QByteArray ts;
        int cc = 0;
        for (int slot = 0; slot < 75; ++slot) {
            if (slot % 10 == 0) {
                ts += tb::psiPacket(0x0000, tb::patSection({{1, 0x1000}, {2, 0x1001}}));
                ts += tb::psiPacket(0x1000, tb::pmtSection(1, 0x1FFF, {{0x1B, 0x100}}));
                ts += tb::psiPacket(0x1001, tb::pmtSection(2, 0x200, {{0x1B, 0x200}}));
            }
            ts += pcrPacket(0x200, static_cast<std::int64_t>(slot * 0.04 * 27e6), slot);
            if (slot == 45) {   // after the 1 s rate window
                ts += tb::tsPacket(0x100, true, tb::pesHeaderWithPts(162000), cc++, 0x40);
            }
            ts += tb::tsPacket(0x100, false, QByteArray(184, char(0)), cc++);
        }
        bool randomAccess = false;
        QString error;
        const bool opened = timedOpen(dir.filePath("no-pcr-pid.fifo"), ts, 1, &randomAccess, &error);
        ok &= expect(opened, "timed windows, no PCR PID: the program starts: " + error.toStdString());
        ok &= expect(randomAccess, "timed windows, no PCR PID: the random access window finds the RAP");
    }

    // A PCR jump inside the rate window of a program of a multiplex: the rate
    // is measured from the jump on, so it stays the program's true rate,
    // which becomes its mpeg2tsMuxRate (MSFTS 6.7). Every 40 ms, program 1
    // sends a PCR and nine video packets, and every 400 ms its PAT and PMT:
    // (250 + 5) packets/s of 188 octets, 383,520 bit/s. Both PCRs jump 100 s
    // ahead at 0.5 s, after the PSI window and inside the 1 s rate window.
    {
        QByteArray ts;
        int cc1 = 0;
        int cc2 = 0;
        for (int slot = 0; slot < 75; ++slot) {
            const double t = slot * 0.04 + (slot >= 13 ? 100.0 : 0.0);
            if (slot % 10 == 0) {
                ts += tb::psiPacket(0x0000, tb::patSection({{1, 0x1000}, {2, 0x1001}}));
                ts += tb::psiPacket(0x1000, tb::pmtSection(1, 0x100, {{0x1B, 0x100}}));
                ts += tb::psiPacket(0x1001, tb::pmtSection(2, 0x200, {{0x1B, 0x200}}));
            }
            ts += pcrPacket(0x100, static_cast<std::int64_t>(t * 27e6), cc1++);
            for (int k = 0; k < 9; ++k) {
                ts += tb::tsPacket(0x100, false, QByteArray(184, char(k)), cc1++);
            }
            ts += pcrPacket(0x200, static_cast<std::int64_t>(t * 27e6), cc2++);
            for (int k = 0; k < 4; ++k) {
                ts += tb::tsPacket(0x200, false, QByteArray(184, char(k)), cc2++);
            }
        }
        bool randomAccess = false;
        QString error;
        qint64 rate = 0;
        const bool opened = timedOpen(dir.filePath("rate-jump.fifo"), ts, 1, &randomAccess, &error, &rate);
        ok &= expect(opened, "timed windows, PCR jump in the rate window: opens: " + error.toStdString());
        ok &= expect(rate > 383520 * 97 / 100 && rate < 383520 * 103 / 100,
                     "timed windows, PCR jump in the rate window: the rate is within 3% of 383,520 bit/s (" +
                         std::to_string(rate) + ")");
    }

    // The null packets count from the PCR jump on too: a one-program source
    // with null stuffing before the jump only is VBR in the window that
    // measures it, so it gives no mux rate.
    {
        QByteArray ts;
        int cc = 0;
        for (int slot = 0; slot < 75; ++slot) {
            const double t = slot * 0.04 + (slot >= 13 ? 100.0 : 0.0);
            if (slot % 10 == 0) {
                ts += tb::psiPacket(0x0000, tb::patSection({{1, 0x1000}}));
                ts += tb::psiPacket(0x1000, tb::pmtSection(1, 0x100, {{0x1B, 0x100}}));
            }
            ts += pcrPacket(0x100, static_cast<std::int64_t>(t * 27e6), cc++);
            for (int k = 0; k < 9; ++k) {
                ts += tb::tsPacket(0x100, false, QByteArray(184, char(k)), cc++);
            }
            if (slot < 13) {
                ts += tb::tsPacket(0x1FFF, false, QByteArray(184, char(0xFF))) +
                      tb::tsPacket(0x1FFF, false, QByteArray(184, char(0xFF)));
            }
        }
        bool randomAccess = false;
        QString error;
        qint64 muxRate = -1;
        const bool opened =
            timedOpen(dir.filePath("rate-jump-nulls.fifo"), ts, 1, &randomAccess, &error, nullptr, &muxRate);
        ok &= expect(opened, "timed windows, nulls before a PCR jump: opens: " + error.toStdString());
        ok &= expect(muxRate == 0, "timed windows, nulls before a PCR jump only: no mux rate (" +
                                       std::to_string(muxRate) + ")");
    }

    // A multiplex without a reference program goes on when the program whose
    // random access points cut Groups leaves the PAT: the catalog names no
    // program. With --program, the catalog names it, so the track ends (draft
    // "Catalog": a new program number needs a new track).
    {
        const QByteArray video = tb::tsPacket(0x100, false, QByteArray(184, char(0)));
        QByteArray ts = tb::psiPacket(0x0000, tb::patSection({{1, 0x1000}, {2, 0x1001}, {3, 0x1002}}, 0), 0);
        ts += tb::psiPacket(0x1000, tb::pmtSection(1, 0x100, {{0x1B, 0x100}})) + video;
        ts += tb::psiPacket(0x0000, tb::patSection({{2, 0x1001}, {3, 0x1002}}, 1), 1) + video;
        const QString path = dir.filePath("reference-leaves.ts");
        ok &= expect(writeFile(path, ts), "write reference-leaves stream");
        const Run run = publish(path, true);
        ok &= expect(run.error.isEmpty() && run.packets.size() == 5, "multiplex without reference: the track goes on");
        const Run named = publish(path, true, 1);
        ok &= expect(named.packets.size() == 3 && named.error.contains("Reference program 1 left"),
                     "multiplex with --program 1: the track ends when program 1 leaves the PAT");

        // Draft "Unmodified Multiplex": MUST NOT be used for a one-program PAT.
        QByteArray single = tb::psiPacket(0x0000, tb::patSection({{1, 0x1000}, {2, 0x1001}}, 0), 0);
        single += tb::psiPacket(0x1000, tb::pmtSection(1, 0x100, {{0x1B, 0x100}})) + video;
        single += tb::psiPacket(0x0000, tb::patSection({{2, 0x1001}}, 1), 1) + video;
        const QString singlePath = dir.filePath("multiplex-to-single.ts");
        ok &= expect(writeFile(singlePath, single), "write multiplex-to-single stream");
        const Run toSingle = publish(singlePath, true);
        ok &= expect(toSingle.packets.size() == 3 && toSingle.error.contains("no longer a multiplex"),
                     "multiplex whose PAT drops to one program: the track ends");

        // Groups go on, on the video of the first program still listed, once its
        // PMT arrives. An audio indicator before that PMT starts no Group.
        const auto rap = [](int pid, int cc) { return tb::tsPacket(pid, true, tb::pesHeaderWithPts(9000 * cc), cc, 0x40); };
        QByteArray moved = tb::psiPacket(0x0000, tb::patSection({{1, 0x1000}, {2, 0x1001}, {3, 0x1002}}, 0), 0);
        moved += tb::psiPacket(0x1000, tb::pmtSection(1, 0x100, {{0x1B, 0x100}})) + rap(0x100, 0) + rap(0x100, 1);
        moved += tb::psiPacket(0x0000, tb::patSection({{2, 0x1001}, {3, 0x1002}}, 1), 1) + rap(0x201, 0);
        moved += tb::psiPacket(0x1001, tb::pmtSection(2, 0x200, {{0x1B, 0x200}, {0x0F, 0x201}})) + rap(0x200, 0) + rap(0x200, 1);
        const QString movedPath = dir.filePath("reference-moves.ts");
        ok &= expect(writeFile(movedPath, moved), "write reference-moves stream");
        M2tsPacketizer packetizer(movedPath);
        packetizer.setTransparent(true);
        QString error;
        ok &= expect(packetizer.open(0, &error), "reference moves: opens");
        M2tsObject object;
        QList<int> groupPids;
        while (packetizer.readObject(1, &object, &error)) {
            if (object.startsGroup) {
                groupPids.append(pidOfPacket(object.payload));
            }
        }
        ok &= expect(groupPids == QList<int>({0x100, 0x100, 0x200, 0x200}),
                     "multiplex: Groups follow the next program's video after the reference program leaves");
    }

    // An SDT that never lists the service is dropped, with one warning.
    {
        const QByteArray sdt = tb::longSection(0x42, 1, tb::sdtBody(0x22, {{7, {}}}));
        QByteArray ts = tb::psiPacket(0x0000, tb::patSection({{1, 0x1000}}));
        ts += tb::psiPacket(0x1000, tb::pmtSection(1, 0x100, {{0x1B, 0x100}}));
        ts += tb::psiPacket(0x0011, sdt, 0) + tb::psiPacket(0x0011, sdt, 1);
        const QString path = dir.filePath("sdt-no-service.ts");
        ok &= expect(writeFile(path, ts), "write SDT-without-service stream");
        qInstallMessageHandler(captureWarning);
        warnings().clear();
        const Run run = publish(path, false, 0, true);
        qInstallMessageHandler(nullptr);
        ok &= expect(!pids(run).contains(0x0011) && warnings().filter("lists no service").size() == 1,
                     "SDT without the service: dropped, one warning");
    }

    // The operational latency warning fires once for GOPs over 2 seconds;
    // MSFTS-02 does not require that limit.
    {
        const auto gops = [](std::uint64_t spacing90k) {
            QByteArray ts = tb::psiPacket(0x0000, tb::patSection({{1, 0x1000}}));
            ts += tb::psiPacket(0x1000, tb::pmtSection(1, 0x100, {{0x1B, 0x100}}));
            for (int gop = 0; gop < 3; ++gop) {
                ts += tb::tsPacket(0x100, true, tb::pesHeaderWithPts(900000 + gop * spacing90k), gop, 0x40);
            }
            return ts;
        };
        qInstallMessageHandler(captureWarning);
        for (const auto& [spacing, expected] : {std::pair{std::uint64_t{180000}, 0}, std::pair{std::uint64_t{270000}, 1}}) {
            warnings().clear();
            const QString path = dir.filePath(QStringLiteral("gop-%1.ts").arg(spacing));
            writeFile(path, gops(spacing));
            publish(path, false);
            const int count = static_cast<int>(warnings().filter("consider a shorter GOP").size());
            ok &= expect(count == expected, "Group duration warning for a spacing of " + std::to_string(spacing / 90) +
                                                " ms: " + std::to_string(count));
        }

        // The Group length runs from one random access point to the next, not
        // from the first video PES of the Object: here a frame 40 ms before the
        // first random access point opens its Object, and the points are 2 s
        // apart.
        warnings().clear();
        QByteArray early = tb::psiPacket(0x0000, tb::patSection({{1, 0x1000}}));
        early += tb::psiPacket(0x1000, tb::pmtSection(1, 0x100, {{0x1B, 0x100}}));
        early += tb::tsPacket(0x100, true, tb::pesHeaderWithPts(900000 - 3600), 0);          // Object 1
        early += tb::tsPacket(0x100, true, tb::pesHeaderWithPts(900000), 1, 0x40);
        early += tb::tsPacket(0x100, false, QByteArray(184, char(0)), 2);                    // Object 2
        early += tb::tsPacket(0x100, true, tb::pesHeaderWithPts(900000 + 180000), 3, 0x40);
        const QString earlyPath = dir.filePath("gop-early-frame.ts");
        writeFile(earlyPath, early);
        M2tsPacketizer earlyPacketizer(earlyPath);
        QString earlyError;
        M2tsObject earlyObject;
        if (earlyPacketizer.open(0, &earlyError)) {
            while (earlyPacketizer.readObject(2, &earlyObject, &earlyError)) {
            }
        }
        ok &= expect(warnings().filter("consider a shorter GOP").isEmpty(),
                     "Group duration: random access points 2 s apart give no warning when a frame precedes one");
        qInstallMessageHandler(nullptr);
    }

    // Object media time: the PTS of the first video PES in each Object, on the
    // PMT's video PID even when an audio PES comes first and the PCR has its own
    // PID. Two packets per Object.
    {
        constexpr std::uint64_t kWrap = std::uint64_t{1} << 33;
        const auto video = [](std::uint64_t pts, int cc) { return tb::tsPacket(0x100, true, tb::pesHeaderWithPts(pts), cc); };
        const auto audio = [](std::uint64_t pts, int cc) { return tb::tsPacket(0x101, true, tb::pesHeaderWithPts(pts), cc); };
        QByteArray ts = tb::psiPacket(0x0000, tb::patSection({{1, 0x1000}}));
        ts += tb::psiPacket(0x1000, tb::pmtSection(1, 0x1FF, {{0x0F, 0x101}, {0x1B, 0x100}}));
        ts += audio(1000, 0) + video(900900, 0);                               // Object 1
        ts += tb::tsPacket(0x100, false, QByteArray(184, char(0)), 1) + audio(2000, 1);  // Object 2
        ts += video(kWrap - 90, 2) + tb::tsPacket(0x1FF, false, QByteArray(184, char(0)), 0);  // Object 3
        ts += video(450, 3) + video(451, 4);                                   // Object 4
        const QString path = dir.filePath("pts.ts");
        ok &= expect(writeFile(path, ts), "write PTS stream");

        for (const bool transparent : {true, false}) {
            const std::string label = transparent ? "transparent" : "filtered";
            M2tsPacketizer packetizer(path);
            packetizer.setTransparent(transparent);
            QString error;
            ok &= expect(packetizer.open(0, &error), label + " PTS stream opens: " + error.toStdString());
            M2tsObject object;
            ok &= expect(packetizer.readObject(2, &object, &error) && !object.ptsUs.has_value(),
                         label + ": PAT and PMT carry no PTS");
            ok &= expect(packetizer.readObject(2, &object, &error) && object.ptsUs == std::uint64_t{10010000},
                         label + ": video PTS, not the audio PTS before it");
            ok &= expect(packetizer.readObject(2, &object, &error) && !object.ptsUs.has_value(),
                         label + ": no video PES start, no PTS");
            ok &= expect(packetizer.readObject(2, &object, &error) && object.ptsUs == (kWrap - 90) * 100 / 9,
                         label + ": PTS before the wrap");
            ok &= expect(packetizer.readObject(2, &object, &error) && object.ptsUs == (kWrap + 450) * 100 / 9,
                         label + ": PTS unwrapped past 2^33");
        }
    }

    // A looped source steps the PTS back; the unwrapped value never goes back by
    // more than the B-frame reordering allowance.
    {
        moq2ts::PtsUnwrapper unwrapper;
        ok &= expect(unwrapper.unwrap(1000000) == 1000000, "unwrap: first value");
        ok &= expect(unwrapper.unwrap(997000) == 997000, "unwrap: B-frame step back kept");
        ok &= expect(unwrapper.unwrap(9000) == 997000, "unwrap: loop does not go back");
        ok &= expect(unwrapper.unwrap(12003) == 1000003, "unwrap: media time continues after the loop");
    }

    // Unmodified multiplex (draft "Unmodified Multiplex", "Use of MSF
    // Initialization Data"): every source packet goes out unchanged, a
    // reference program is named only on request, and initData carries the PAT
    // with the PMT of the reference program, or of the first program in the PAT
    // (a moq2ts choice) when none is named.
    {
        const QByteArray mux = stream({{1, 0x1000}, {2, 0x1001}});
        const QString path = dir.filePath("mux.ts");
        ok &= expect(writeFile(path, mux), "write multiplex");

        M2tsPacketizer unnamed(path);
        unnamed.setTransparent(true);
        QString error;
        ok &= expect(unnamed.open(0, &error), "multiplex opens: " + error.toStdString());
        ok &= expect(!unnamed.namesProgram() && !unnamed.randomAccess(), "multiplex without --program names no program");
        ok &= expect(unnamed.initData() == mux.left(2 * 188),
                     "multiplex initData: the PAT and the PMT of the first program");
        QByteArray out;
        M2tsObject object;
        while (unnamed.readObject(4, &object, &error)) {
            out += object.payload;
        }
        ok &= expect(out == mux, "multiplex file: byte-for-byte");

        M2tsPacketizer named(path);
        named.setTransparent(true);
        ok &= expect(named.open(2, &error), "multiplex with --program 2 opens: " + error.toStdString());
        ok &= expect(named.namesProgram() && named.programNumber() == 2 && named.pcrPid() == 0x100,
                     "multiplex with --program 2 names program 2");
        ok &= expect(named.initData() == mux.left(188) + mux.mid(2 * 188, 188),
                     "multiplex with --program 2: initData is the PAT and the PMT of program 2");

        M2tsPacketizer missing(path);
        missing.setTransparent(true);
        ok &= expect(!missing.open(9, &error) && error.contains("program 9"),
                     "multiplex with --program 9: refused, the PAT does not list it");

        const LiveRun live = publishLive(dir.filePath("mux-live.fifo"), mux, true);
        QByteArray liveOut;
        for (const QByteArray& payload : live.run.packets) {
            liveOut += payload;
        }
        ok &= expect(live.run.error.isEmpty() && liveOut == mux && live.initData == mux.left(2 * 188),
                     "live multiplex: byte-for-byte, initData with the PMT of the first program");
    }

    // Without a PAT, the unmodified mode cannot be chosen, so the track is
    // refused.
    {
        QByteArray ts;
        for (int cc = 0; cc < 4; ++cc) {
            ts += tb::tsPacket(0x100, false, QByteArray(184, char(0)), cc);
        }
        const QString path = dir.filePath("no-pat.ts");
        ok &= expect(writeFile(path, ts), "write stream without a PAT");
        const Run run = publish(path, true);
        ok &= expect(run.packets.isEmpty() && run.error.contains("No PAT"), "unmodified without a PAT: refused");
    }

    // A multiplex without a reference program: initData is the PAT and the PMT
    // of the first program in the PAT. A new PMT of that program updates it, a
    // new PMT of another program does not, and when the first program leaves
    // the PAT the next one takes over. initData that was missing at open()
    // stays missing: a later catalog cannot add initRef (MSF Section 5.3).
    {
        const QByteArray pat = tb::psiPacket(0x0000, tb::patSection({{1, 0x1000}, {2, 0x1001}}));
        const QByteArray pmt1 = tb::psiPacket(0x1000, tb::pmtSection(1, 0x100, {{0x1B, 0x100}}));
        const QByteArray pmt1v1 = tb::psiPacket(0x1000, tb::pmtSection(1, 0x100, {{0x1B, 0x100}, {0x0F, 0x101}}, 1), 1);
        const QByteArray pmt2 = tb::psiPacket(0x1001, tb::pmtSection(2, 0x200, {{0x1B, 0x200}}), 0);
        const QByteArray pmt2v1 = tb::psiPacket(0x1001, tb::pmtSection(2, 0x200, {{0x1B, 0x200}, {0x0F, 0x201}}, 1), 1);
        const QByteArray video = tb::tsPacket(0x100, false, QByteArray(184, char(0)));
        const auto changes = [&](const QString& name, const QByteArray& ts, QByteArray* initData) {
            const QString path = dir.filePath(name);
            writeFile(path, ts);
            M2tsPacketizer packetizer(path);
            packetizer.setTransparent(true);
            QString error;
            packetizer.open(0, &error);
            M2tsObject object;
            bool changed = false;
            while (packetizer.readObject(1, &object, &error)) {
                changed |= packetizer.takeInitDataChange(initData);
            }
            return changed;
        };
        QByteArray initData;
        ok &= expect(changes("mux-pmt1-change.ts", pat + pmt1 + pmt2 + video + pmt1v1 + video, &initData) &&
                         initData == pat + pmt1v1,
                     "multiplex: a new PMT of the first program updates initData");
        ok &= expect(!changes("mux-pmt2-change.ts", pat + pmt1 + pmt2 + video + pmt2v1 + video, &initData),
                     "multiplex: a new PMT of another program leaves initData unchanged");

        const QByteArray pat3 = tb::psiPacket(0x0000, tb::patSection({{1, 0x1000}, {2, 0x1001}, {3, 0x1002}}, 0), 0);
        const QByteArray pat23 = tb::psiPacket(0x0000, tb::patSection({{2, 0x1001}, {3, 0x1002}}, 1), 1);
        const QByteArray pmt3 = tb::psiPacket(0x1002, tb::pmtSection(3, 0x300, {{0x1B, 0x300}}));
        ok &= expect(changes("mux-first-leaves.ts", pat3 + pmt1 + pmt2 + pmt3 + video + pat23 + pmt2 + video, &initData) &&
                         initData == pat23 + pmt2,
                     "multiplex: when the first program leaves, initData carries the PMT of the next first program");

        QByteArray late = pat + pmt2;
        for (int index = 0; index < 20000; ++index) {
            late += tb::tsPacket(0x1FFF, false, QByteArray(184, char(0xFF)));
        }
        late += pmt1 + video;
        const QString latePath = dir.filePath("mux-late-pmt.ts");
        ok &= expect(writeFile(latePath, late), "write multiplex with a late PMT");
        M2tsPacketizer latePacketizer(latePath);
        latePacketizer.setTransparent(true);
        QString error;
        ok &= expect(latePacketizer.open(0, &error) && latePacketizer.initData().isEmpty(),
                     "multiplex: no initData while the PMT of the first program is missing at open");
        M2tsObject object;
        bool changed = false;
        while (latePacketizer.readObject(64, &object, &error)) {
            changed |= latePacketizer.takeInitDataChange(&initData);
        }
        ok &= expect(!changed, "multiplex: initData missing at open is not added later");
    }

    // Each Object reports the first PCR of the program's PCR PID (PID 0x100), or
    // -1, which is what --paced releases the Object by.
    {
        constexpr std::int64_t first = 27000000 * 5 + 123;      // not a multiple of 300
        constexpr std::int64_t second = first + 27000000 / 4;
        const QByteArray video = tb::tsPacket(0x100, false, QByteArray(184, char(0x00)));
        QByteArray ts = tb::psiPacket(0x0000, tb::patSection({{1, 0x1000}})) +
                        tb::psiPacket(0x1000, tb::pmtSection(1, 0x100, {{0x1B, 0x100}}));
        // PCR on another PID: not the program's clock.
        const QByteArray other = pcrPacket(0x101, 999999999, 0);
        ts += video + video + other + video;
        ts += pcrPacket(0x100, first, 0) + video + video + video;
        ts += video + video + video + video;
        ts += pcrPacket(0x100, second, 1) + video + video + video;
        const QString path = dir.filePath("pcr.ts");
        ok &= expect(writeFile(path, ts), "write PCR stream");

        M2tsPacketizer packetizer(path);
        QString error;
        ok &= expect(packetizer.open(0, &error), "PCR stream opens: " + error.toStdString());
        QList<std::int64_t> pcrs;
        M2tsObject object;
        while (packetizer.readObject(4, &object, &error)) {
            pcrs.append(object.pcr27);
        }
        // PAT, PMT, video, video | other-PID PCR, video, first, video | video x4 |
        // video, video, second, video | video x2 (rest)
        std::int64_t seen = 0;
        QList<std::int64_t> found;
        for (const std::int64_t value : pcrs) {
            if (value >= 0) {
                found.append(value);
            }
            seen += 1;
        }
        ok &= expect(found == QList<std::int64_t>({first, second}),
                     "PCR: the PCR PID's PCRs are reported, in order, to the nearest 27 MHz tick");
        ok &= expect(pcrs.size() == seen && pcrs.first() == -1, "PCR: an Object without one reports -1");

        // One Object holding both: the first.
        M2tsPacketizer whole(path);
        ok &= expect(whole.open(0, &error), "PCR stream opens again");
        ok &= expect(whole.readObject(64, &object, &error) && object.pcr27 == first,
                     "PCR: an Object with two reports the first");
        // A PCR on another PID alone does not count.
        const QString otherPath = dir.filePath("pcr-other.ts");
        ok &= expect(writeFile(otherPath, tb::psiPacket(0x0000, tb::patSection({{1, 0x1000}})) +
                                              tb::psiPacket(0x1000, tb::pmtSection(1, 0x100, {{0x1B, 0x100}})) + video +
                                              other + video),
                     "write other-PID PCR stream");
        M2tsPacketizer ignored(otherPath);
        ok &= expect(ignored.open(0, &error) && ignored.readObject(64, &object, &error) && object.pcr27 == -1,
                     "PCR: a PCR on another PID is not the clock");
    }

    if (ok) {
        std::cout << "m2ts packetizer tests passed\n";
    }
    return ok ? 0 : 1;
}
