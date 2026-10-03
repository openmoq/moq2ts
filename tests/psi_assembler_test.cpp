#include "media/M2tsPacketizer.h"
#include "ts_test_builder.h"

#include <iostream>
#include <string>

using moq2ts::PsiAssembler;
namespace tb = moq2ts::test;

namespace {
bool expect(bool cond, const std::string& msg) {
    if (!cond) { std::cerr << "FAIL: " << msg << '\n'; return false; }
    return true;
}

QList<PsiAssembler::Section> feed(PsiAssembler* assembler, const QList<QByteArray>& packets) {
    QList<PsiAssembler::Section> sections;
    for (const QByteArray& packet : packets) {
        sections += assembler->push(packet, packet);
    }
    return sections;
}

// A PMT with 100 elementary streams: 516 octets, three packets.
QByteArray longPmt() {
    QList<std::pair<int, int>> streams;
    for (int index = 0; index < 100; ++index) {
        streams.append({0x06, 0x200 + index});
    }
    return tb::pmtSection(1, 0x200, streams);
}
}  // namespace

int main() {
    bool ok = true;

    // A section over three packets comes out once, with its three packets.
    {
        PsiAssembler assembler;
        const QByteArray section = longPmt();
        const QList<QByteArray> packets = tb::psiPackets(0x1000, section);
        ok &= expect(packets.size() == 3, "long PMT spans three packets");
        const auto sections = feed(&assembler, packets);
        ok &= expect(sections.size() == 1 && sections.at(0).bytes == section, "long PMT reassembled");
        ok &= expect(sections.size() == 1 && sections.at(0).sourcePackets == packets, "long PMT keeps its packets");
    }

    // Two sections in one packet both come out.
    {
        PsiAssembler assembler;
        const QByteArray first = tb::pmtSection(1, 0x100, {{0x1B, 0x100}});
        const QByteArray second = tb::pmtSection(2, 0x200, {{0x1B, 0x200}});
        QByteArray payload;
        payload.append(char(0x00));
        payload += first + second;
        const auto sections = feed(&assembler, {tb::tsPacket(0x1000, true, payload)});
        ok &= expect(sections.size() == 2 && sections.at(0).bytes == first && sections.at(1).bytes == second,
                     "two sections in one packet");
    }

    // A section whose 3-octet header is split across two packets, closed by the
    // pointer_field of a packet that then starts the next section.
    {
        PsiAssembler assembler;
        const QByteArray filler = tb::pmtSection(9, 0x100, QList<std::pair<int, int>>(33, {0x06, 0x300}));
        const QByteArray split = tb::pmtSection(1, 0x100, {{0x1B, 0x100}});
        const QByteArray next = tb::pmtSection(2, 0x200, {{0x1B, 0x200}});
        // Packet 1: pointer 0, the filler section, then the first 2 octets of split.
        QByteArray first;
        first.append(char(0x00));
        first += filler;
        const int room = 184 - static_cast<int>(first.size());
        ok &= expect(room > 0 && room < 3, "test layout leaves 1 or 2 octets");
        first += split.left(room);
        // Packet 2: pointer to the end of split, then the next section.
        QByteArray second;
        const QByteArray tail = split.mid(room);
        second.append(static_cast<char>(tail.size()));
        second += tail + next;
        const auto sections = feed(&assembler, {tb::tsPacket(0x1000, true, first, 0), tb::tsPacket(0x1000, true, second, 1)});
        ok &= expect(sections.size() == 3 && sections.at(1).bytes == split && sections.at(2).bytes == next,
                     "split header and pointer_field");
        ok &= expect(sections.size() == 3 && sections.at(1).sourcePackets.size() == 2, "split section has two packets");
    }

    // A bad CRC_32 is dropped.
    {
        PsiAssembler assembler;
        QByteArray section = tb::pmtSection(1, 0x100, {{0x1B, 0x100}});
        section[section.size() - 1] = static_cast<char>(section.at(section.size() - 1) ^ 0x01);
        ok &= expect(feed(&assembler, tb::psiPackets(0x1000, section)).isEmpty(), "bad CRC_32 dropped");
    }

    // A lost packet in the middle of a section drops it; the next one survives.
    {
        PsiAssembler assembler;
        QList<QByteArray> packets = tb::psiPackets(0x1000, longPmt(), 0);
        packets.removeAt(1);
        const QByteArray after = tb::pmtSection(1, 0x100, {{0x1B, 0x100}});
        packets += tb::psiPackets(0x1000, after, 3);
        const auto sections = feed(&assembler, packets);
        ok &= expect(sections.size() == 1 && sections.at(0).bytes == after, "CC gap drops the broken section");
    }

    // A section without CRC_32 (section_syntax_indicator 0) relies on the
    // continuity counter alone: after a lost packet, later bytes must not
    // complete it.
    {
        PsiAssembler assembler;
        QByteArray section;
        section.append(char(0x80));            // private table_id
        section.append(char(0x71));            // syntax 0, private 1, length 400
        section.append(char(0x90));
        section.append(QByteArray(400, char(0x55)));
        QList<QByteArray> packets = tb::psiPackets(0x1000, section, 0);
        ok &= expect(packets.size() == 3, "private section spans three packets");
        ok &= expect(feed(&assembler, packets).size() == 1, "private section without loss");
        PsiAssembler lossy;
        packets.removeAt(1);
        packets.append(tb::tsPacket(0x1000, false, QByteArray(184, char(0x55)), 3));
        ok &= expect(feed(&lossy, packets).isEmpty(), "CC gap drops a section without CRC_32");
    }

    // A duplicate packet is ignored.
    {
        PsiAssembler assembler;
        QList<QByteArray> packets = tb::psiPackets(0x1000, longPmt(), 0);
        packets.insert(1, packets.at(0));
        ok &= expect(feed(&assembler, packets).size() == 1, "duplicate packet ignored");
    }

    // SectionRewriter: drop, keep, and replace. A replaced section gets version
    // 0, keeps it while the content stays, and moves to 1 when it changes. Output
    // packets have their own continuity counter; a long section spans packets.
    {
        moq2ts::SectionRewriter rewriter;
        using Decision = moq2ts::SectionRewriter::Decision;
        const QByteArray keep = tb::pmtSection(1, 0x100, {{0x1B, 0x100}});
        const QByteArray drop = tb::pmtSection(2, 0x100, {{0x1B, 0x100}});
        const QByteArray replaceA = tb::pmtSection(3, 0x100, {{0x1B, 0x100}}, 7);
        const QByteArray replaceB = tb::pmtSection(3, 0x100, {{0x1B, 0x100}, {0x0F, 0x101}}, 7);
        int cc = 0;
        QList<QByteArray> out;
        const auto feed = [&](const QByteArray& section, const moq2ts::SectionRewriter::Filter& filter) {
            for (const QByteArray& packet : tb::psiPackets(0x1000, section, cc)) {
                out += rewriter.push(packet, packet, filter);
                cc = (cc + 1) & 0x0F;
            }
        };
        const auto byProgram = [&](const QByteArray& section) {
            const int program = (static_cast<unsigned char>(section[3]) << 8) | static_cast<unsigned char>(section[4]);
            if (program == 1) return Decision{Decision::Keep, {}};
            if (program == 3) return Decision{Decision::Replace, section};
            return Decision{};
        };
        feed(keep, byProgram);
        feed(drop, byProgram);
        feed(replaceA, byProgram);
        feed(replaceA, byProgram);
        feed(replaceB, byProgram);
        feed(longPmt(), [](const QByteArray&) { return Decision{Decision::Keep, {}}; });
        PsiAssembler check;
        QList<QByteArray> sections;
        for (int index = 0; index < out.size(); ++index) {
            ok &= expect((static_cast<unsigned char>(out.at(index)[3]) & 0x0F) == (index & 0x0F), "rewriter: own CC");
            sections += [&] { QList<QByteArray> got; for (const auto& section : check.push(out.at(index), out.at(index))) got.append(section.bytes); return got; }();
        }
        ok &= expect(sections.size() == 5, "rewriter: dropped section absent, others present");
        ok &= expect(sections.value(0) == keep, "rewriter: kept section unchanged");
        ok &= expect(sections.value(1) == tb::pmtSection(3, 0x100, {{0x1B, 0x100}}, 0) &&
                         sections.value(2) == sections.value(1),
                     "rewriter: replaced section gets version 0, stable on repeat");
        ok &= expect(sections.value(3) == tb::pmtSection(3, 0x100, {{0x1B, 0x100}, {0x0F, 0x101}}, 1),
                     "rewriter: new content moves to version 1, with a valid CRC_32");
        ok &= expect(sections.value(4) == longPmt() && out.size() == 7, "rewriter: a long section spans three packets");
    }

    // The production CRC_32 matches the builder's.
    {
        const QByteArray data("123456789");
        ok &= expect(moq2ts::mpegCrc32(data.constData(), data.size()) == tb::mpegCrc32(data) &&
                         tb::mpegCrc32(data) == 0x0376E6E7u,
                     "CRC-32/MPEG-2 check value");
    }

    if (ok) {
        std::cout << "psi assembler tests passed\n";
    }
    return ok ? 0 : 1;
}
