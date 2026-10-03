#include "media/M2tsPacketizer.h"
#include "ts_test_builder.h"

#include <QDir>
#include <QFile>
#include <QMap>
#include <QStringList>
#include <QtGlobal>
#include <QTemporaryDir>

#include <sys/stat.h>

#include <cstdint>
#include <iostream>
#include <string>
#include <thread>

using moq2ts::M2tsObject;
using moq2ts::M2tsPacketizer;
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

LiveRun publishLive(const QString& fifo, const QByteArray& data, bool transparent) {
    LiveRun live;
    ::mkfifo(fifo.toLocal8Bit().constData(), 0600);
    std::thread writer([&] { writeFile(fifo, data); });
    M2tsPacketizer packetizer(fifo);
    packetizer.setTransparent(transparent);
    if (packetizer.open(0, &live.run.error)) {
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

int main() {
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

        // A live multiplex with a reference program (the first of the PAT,
        // whose PMT is known) starts at that program's random access point and
        // declares random access.
        const QByteArray multiplex = stream({{1, 0x1000}, {2, 0x1001}});
        const LiveRun live = publishLive(dir.filePath("live-m.fifo"), multiplex, true);
        ok &= expect(live.randomAccess && !live.run.packets.isEmpty() && live.run.packets.first() == rap &&
                         live.firstStartsGroup && live.initData.isEmpty(),
                     "live multiplex: starts at the reference program's RAP, declares random access");
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
    }

    // A multiplex track goes on when its reference program leaves the PAT: the
    // multiplex is still valid, and the reference fields are advisory.
    {
        const QByteArray video = tb::tsPacket(0x100, false, QByteArray(184, char(0)));
        QByteArray ts = tb::psiPacket(0x0000, tb::patSection({{1, 0x1000}, {2, 0x1001}}, 0), 0);
        ts += tb::psiPacket(0x1000, tb::pmtSection(1, 0x100, {{0x1B, 0x100}})) + video;
        ts += tb::psiPacket(0x0000, tb::patSection({{2, 0x1001}}, 1), 1) + video;
        const QString path = dir.filePath("reference-leaves.ts");
        ok &= expect(writeFile(path, ts), "write reference-leaves stream");
        const Run run = publish(path, true);
        ok &= expect(run.error.isEmpty() && run.packets.size() == 5, "multiplex: the track goes on");

        // Groups go on, on the video of the first program still listed, once its
        // PMT arrives. An audio indicator before that PMT starts no Group.
        const auto rap = [](int pid, int cc) { return tb::tsPacket(pid, true, tb::pesHeaderWithPts(9000 * cc), cc, 0x40); };
        QByteArray moved = tb::psiPacket(0x0000, tb::patSection({{1, 0x1000}, {2, 0x1001}}, 0), 0);
        moved += tb::psiPacket(0x1000, tb::pmtSection(1, 0x100, {{0x1B, 0x100}})) + rap(0x100, 0) + rap(0x100, 1);
        moved += tb::psiPacket(0x0000, tb::patSection({{2, 0x1001}}, 1), 1) + rap(0x201, 0);
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

    // Groups longer than 2 seconds give one warning; Groups 2 seconds apart do
    // not.
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
            const int count = static_cast<int>(warnings().filter("longer than the 2 s").size());
            ok &= expect(count == expected, "Group duration warning for a spacing of " + std::to_string(spacing / 90) +
                                                " ms: " + std::to_string(count));
        }
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

    if (ok) {
        std::cout << "m2ts packetizer tests passed\n";
    }
    return ok ? 0 : 1;
}
