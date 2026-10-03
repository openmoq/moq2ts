#include "M2tsPacketizer.h"

#include <algorithm>
#include <vector>

#ifdef MOQ2TS_HAVE_LIBAV_CAPTURE
extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
}
#endif

namespace moq2ts {

namespace {

int pidOf(const QByteArray& tsPacket) {
    if (tsPacket.size() < 4) {
        return -1;
    }
    return ((static_cast<unsigned char>(tsPacket[1]) & 0x1f) << 8) |
           static_cast<unsigned char>(tsPacket[2]);
}

bool payloadUnitStart(const QByteArray& tsPacket) {
    return tsPacket.size() >= 2 && (static_cast<unsigned char>(tsPacket[1]) & 0x40) != 0;
}

int payloadOffset(const QByteArray& tsPacket) {
    if (tsPacket.size() < 4) {
        return -1;
    }
    const int adaptationControl = (static_cast<unsigned char>(tsPacket[3]) >> 4) & 0x03;
    if (adaptationControl == 0 || adaptationControl == 2) {
        return -1;
    }
    int offset = 4;
    if (adaptationControl == 3) {
        if (tsPacket.size() < 5) {
            return -1;
        }
        offset += 1 + static_cast<unsigned char>(tsPacket[4]);
    }
    return offset < tsPacket.size() ? offset : -1;
}

// The (program_number, PMT PID) entries of a PAT section, and apart from them
// the network PID (program_number 0), -1 when absent. Returns false when the
// section is not a PAT.
bool parsePat(const QByteArray& section, std::vector<std::pair<int, int>>* programs, int* networkPid) {
    programs->clear();
    *networkPid = -1;
    if (section.size() < 12 || static_cast<unsigned char>(section[0]) != 0x00) {
        return false;
    }
    const int sectionLength = ((static_cast<unsigned char>(section[1]) & 0x0f) << 8) |
                              static_cast<unsigned char>(section[2]);
    const int entriesEnd = std::min<int>(3 + sectionLength - 4, section.size());
    for (int offset = 8; offset + 4 <= entriesEnd; offset += 4) {
        const int program = (static_cast<unsigned char>(section[offset]) << 8) |
                            static_cast<unsigned char>(section[offset + 1]);
        const int pid = ((static_cast<unsigned char>(section[offset + 2]) & 0x1f) << 8) |
                        static_cast<unsigned char>(section[offset + 3]);
        if (program != 0) {
            programs->emplace_back(program, pid);
        } else {
            *networkPid = pid;
        }
    }
    return true;
}

// The requested program of a PAT, or its first program when requestedProgram is 0.
bool findPatProgram(const std::vector<std::pair<int, int>>& programs, int requestedProgram,
                    int* programNumber, int* pmtPid) {
    for (const auto& [program, pid] : programs) {
        if (requestedProgram == 0 || requestedProgram == program) {
            *programNumber = program;
            *pmtPid = pid;
            return true;
        }
    }
    return false;
}

// Video stream_type values (ISO/IEC 13818-1 Table 2-34): MPEG-1, MPEG-2,
// MPEG-4 Part 2, H.264, H.265, and H.266.
bool isVideoStreamType(int streamType) {
    switch (streamType) {
    case 0x01: case 0x02: case 0x10: case 0x1B: case 0x24: case 0x33:
        return true;
    default:
        return false;
    }
}

// The CA_descriptors (tag 0x09) in the descriptor loop [from, to): their
// CA_PIDs and CA_system_ids.
void readCaDescriptors(const QByteArray& section, int from, int to, std::set<int>* caPids, std::set<int>* caSystems) {
    for (int offset = from; offset + 2 <= to;) {
        const int tag = static_cast<unsigned char>(section[offset]);
        const int length = static_cast<unsigned char>(section[offset + 1]);
        if (tag == 0x09 && length >= 4 && offset + 2 + length <= to) {
            caSystems->insert((static_cast<unsigned char>(section[offset + 2]) << 8) |
                              static_cast<unsigned char>(section[offset + 3]));
            caPids->insert(((static_cast<unsigned char>(section[offset + 4]) & 0x1F) << 8) |
                           static_cast<unsigned char>(section[offset + 5]));
        }
        offset += 2 + length;
    }
}

// The PMT fields the packetizer uses. The CA_descriptors of the program loop
// and of each ES loop give the ECM PIDs and the CA systems in use.
bool parsePmt(const QByteArray& section, int* pcrPid, std::set<int>* elementaryPids, int* videoPid,
              std::set<int>* ecmPids, std::set<int>* caSystems) {
    if (section.size() < 16 || static_cast<unsigned char>(section[0]) != 0x02) {
        return false;
    }
    const int sectionLength = ((static_cast<unsigned char>(section[1]) & 0x0f) << 8) |
                              static_cast<unsigned char>(section[2]);
    const int sectionEnd = 3 + sectionLength - 4;
    if (sectionEnd > section.size() || sectionEnd < 12) {
        return false;
    }

    elementaryPids->clear();
    ecmPids->clear();
    caSystems->clear();
    *videoPid = -1;
    *pcrPid = ((static_cast<unsigned char>(section[8]) & 0x1f) << 8) |
              static_cast<unsigned char>(section[9]);
    const int programInfoLength = ((static_cast<unsigned char>(section[10]) & 0x0f) << 8) |
                                  static_cast<unsigned char>(section[11]);
    readCaDescriptors(section, 12, std::min(12 + programInfoLength, sectionEnd), ecmPids, caSystems);
    int offset = 12 + programInfoLength;
    while (offset + 5 <= sectionEnd) {
        const int streamType = static_cast<unsigned char>(section[offset]);
        const int elementaryPid = ((static_cast<unsigned char>(section[offset + 1]) & 0x1f) << 8) |
                                  static_cast<unsigned char>(section[offset + 2]);
        const int esInfoLength = ((static_cast<unsigned char>(section[offset + 3]) & 0x0f) << 8) |
                                 static_cast<unsigned char>(section[offset + 4]);
        elementaryPids->insert(elementaryPid);
        readCaDescriptors(section, offset + 5, std::min(offset + 5 + esInfoLength, sectionEnd), ecmPids, caSystems);
        if (*videoPid < 0 && isVideoStreamType(streamType)) {
            *videoPid = elementaryPid;
        }
        offset += 5 + esInfoLength;
    }
    return true;
}

} // namespace

std::uint32_t mpegCrc32(const char* data, qsizetype size) {
    std::uint32_t crc = 0xFFFFFFFFu;
    for (qsizetype index = 0; index < size; ++index) {
        crc ^= static_cast<std::uint32_t>(static_cast<unsigned char>(data[index])) << 24;
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc & 0x80000000u) ? (crc << 1) ^ 0x04C11DB7u : crc << 1;
        }
    }
    return crc;
}

void PsiAssembler::reset() {
    m_section.clear();
    m_sectionLength = -1;
    m_packets.clear();
    m_collecting = false;
    m_packetRecorded = false;
    m_lastCc = -1;
}

QList<PsiAssembler::Section> PsiAssembler::push(const QByteArray& tsPacket, const QByteArray& sourcePacket) {
    QList<Section> done;
    const int offset = payloadOffset(tsPacket);
    if (offset < 0) {
        return done;   // no payload; the continuity counter does not advance
    }
    const int cc = static_cast<unsigned char>(tsPacket[3]) & 0x0F;
    if (m_lastCc >= 0 && cc == m_lastCc) {
        return done;   // a duplicate packet (ISO/IEC 13818-1 Section 2.4.3.3)
    }
    if (m_lastCc >= 0 && cc != ((m_lastCc + 1) & 0x0F)) {
        m_collecting = false;   // a lost packet breaks the section in progress
    }
    m_lastCc = cc;
    m_packetRecorded = false;

    const char* payload = tsPacket.constData() + offset;
    const int size = static_cast<int>(tsPacket.size()) - offset;
    if (!payloadUnitStart(tsPacket)) {
        if (m_collecting) {
            append(payload, size, sourcePacket, &done);
        }
        return done;
    }
    // The pointer_field counts the bytes that end the section in progress.
    const int pointer = static_cast<unsigned char>(payload[0]);
    if (1 + pointer > size) {
        m_collecting = false;
        return done;
    }
    if (m_collecting) {
        append(payload + 1, pointer, sourcePacket, &done);
        m_collecting = false;   // a section still incomplete here is corrupt
    }
    // New sections follow until the packet ends or stuffing (0xFF) begins.
    int position = 1 + pointer;
    while (position < size && static_cast<unsigned char>(payload[position]) != 0xFF) {
        m_collecting = true;
        m_section.clear();
        m_sectionLength = -1;
        m_packets.clear();
        m_packetRecorded = false;
        position += append(payload + position, size - position, sourcePacket, &done);
        if (m_collecting) {
            break;   // the section continues in the next packet
        }
    }
    return done;
}

int PsiAssembler::append(const char* data, int size, const QByteArray& sourcePacket, QList<Section>* done) {
    int used = 0;
    while (used < size && m_collecting) {
        // Up to the 3-octet header first, then up to the section_length it gives.
        const int target = m_sectionLength < 0 ? 3 : m_sectionLength;
        const int take = std::min(target - static_cast<int>(m_section.size()), size - used);
        m_section.append(data + used, take);
        used += take;
        if (!m_packetRecorded) {
            m_packets.append(sourcePacket);
            m_packetRecorded = true;
        }
        if (m_section.size() < 3) {
            continue;
        }
        if (m_sectionLength < 0) {
            m_sectionLength = 3 + (((static_cast<unsigned char>(m_section[1]) & 0x0F) << 8) |
                                   static_cast<unsigned char>(m_section[2]));
        }
        const int length = m_sectionLength;
        if (length > 4096) {
            m_collecting = false;   // longer than any PSI or private section
        } else if (m_section.size() == length) {
            m_collecting = false;
            // With section_syntax_indicator set, the section ends with a CRC_32,
            // and the CRC over the whole section is then 0.
            const bool syntax = (static_cast<unsigned char>(m_section[1]) & 0x80) != 0;
            if (!syntax || (length >= 8 && mpegCrc32(m_section.constData(), length) == 0)) {
                done->append({m_section, m_packets});
            }
        }
    }
    return used;
}

int startingSectionNumber(const QByteArray& tsPacket) {
    // The section_number of the section that starts in a packet with
    // payload_unit_start_indicator set, or -1 when its header is not in the
    // packet.
    const int offset = payloadOffset(tsPacket);
    if (offset < 0 || !payloadUnitStart(tsPacket)) {
        return -1;
    }
    const int start = offset + 1 + static_cast<unsigned char>(tsPacket[offset]);
    return start + 6 < tsPacket.size() ? static_cast<unsigned char>(tsPacket[start + 6]) : -1;
}

QList<QByteArray> SectionRewriter::push(const QByteArray& tsPacket, const QByteArray& sourcePacket,
                                       const Filter& filter) {
    QList<QByteArray> out;
    for (const PsiAssembler::Section& section : m_assembler.push(tsPacket, sourcePacket)) {
        const Decision decision = filter(section.bytes);
        if (decision.kind == Decision::Keep) {
            out += packets(section.bytes, tsPacket, sourcePacket);
        } else if (decision.kind == Decision::Replace) {
            out += packets(versioned(decision.section), tsPacket, sourcePacket);
        }
    }
    return out;
}

QByteArray SectionRewriter::versioned(QByteArray section) {
    // A long-form section: version_number sits in bits 5 to 1 of octet 5, and
    // the section ends with its CRC_32. section_length must already count the
    // CRC_32 octets.
    const int length = 3 + (((static_cast<unsigned char>(section[1]) & 0x0F) << 8) |
                            static_cast<unsigned char>(section[2]));
    section.resize(length);   // drops a stale CRC_32 beyond the length, or makes room
    QByteArray content = section.left(length - 4);
    content[5] = static_cast<char>(static_cast<unsigned char>(content[5]) & 0xC1);
    const std::pair<int, int> key{static_cast<unsigned char>(section[0]),
                                  (static_cast<unsigned char>(section[3]) << 8) | static_cast<unsigned char>(section[4])};
    auto& [lastContent, version] = m_versions.try_emplace(key, QByteArray(), -1).first->second;
    if (content != lastContent) {
        lastContent = content;
        version = (version + 1) & 0x1F;   // 0 for the first table
    }
    section[5] = static_cast<char>((static_cast<unsigned char>(section[5]) & 0xC1) | (version << 1));
    const std::uint32_t crc = mpegCrc32(section.constData(), length - 4);
    for (int index = 0; index < 4; ++index) {
        section[length - 4 + index] = static_cast<char>((crc >> (24 - 8 * index)) & 0xFF);
    }
    return section;
}

QList<QByteArray> SectionRewriter::packets(const QByteArray& section, const QByteArray& tsPacket,
                                           const QByteArray& sourcePacket) {
    // Each section starts a packet, with pointer_field 0, and the last packet
    // ends with 0xFF stuffing. A 192-octet packet keeps the prefix of the
    // source packet that completed the section.
    QList<QByteArray> out;
    const QByteArray prefix = sourcePacket.left(sourcePacket.size() - 188);
    qsizetype position = 0;
    while (position < section.size() || out.isEmpty()) {
        QByteArray packet = prefix;
        packet.append(char(0x47));
        packet.append(static_cast<char>((out.isEmpty() ? 0x40 : 0x00) | (static_cast<unsigned char>(tsPacket[1]) & 0x1F)));
        packet.append(tsPacket[2]);
        packet.append(static_cast<char>(0x10 | m_continuityCounter));
        m_continuityCounter = (m_continuityCounter + 1) & 0x0F;
        if (out.isEmpty()) {
            packet.append(char(0x00));   // pointer_field
        }
        const qsizetype room = prefix.size() + 188 - packet.size();
        packet.append(section.mid(position, room));
        position += room;
        packet.append(QByteArray(prefix.size() + 188 - packet.size(), char(0xFF)));
        out.append(packet);
    }
    return out;
}

std::int64_t pcrOf(const QByteArray& tsPacket) {
    // adaptation_field_control 2 or 3, adaptation_field_length of at least 7,
    // and PCR_flag (0x10): program_clock_reference_base (33 bits) and _extension
    // (9 bits).
    if (tsPacket.size() < 12 || ((static_cast<unsigned char>(tsPacket[3]) >> 4) & 0x02) == 0 ||
        static_cast<unsigned char>(tsPacket[4]) < 7 || (static_cast<unsigned char>(tsPacket[5]) & 0x10) == 0) {
        return -1;
    }
    const auto byte = [&](int index) { return static_cast<std::int64_t>(static_cast<unsigned char>(tsPacket[index])); };
    const std::int64_t base = (byte(6) << 25) | (byte(7) << 17) | (byte(8) << 9) | (byte(9) << 1) | (byte(10) >> 7);
    const std::int64_t extension = ((byte(10) & 0x01) << 8) | byte(11);
    return base * 300 + extension;
}

std::int64_t pesPts(const QByteArray& tsPacket) {
    if (!payloadUnitStart(tsPacket)) {
        return -1;
    }
    const int offset = payloadOffset(tsPacket);
    if (offset < 0 || offset + 14 > tsPacket.size()) {
        return -1;
    }
    const auto byte = [&](int index) { return static_cast<std::int64_t>(static_cast<unsigned char>(tsPacket[offset + index])); };
    if (byte(0) != 0x00 || byte(1) != 0x00 || byte(2) != 0x01) {
        return -1;
    }
    // '10' marker bits, then PTS_DTS_flags '10' or '11'.
    if ((byte(6) & 0xC0) != 0x80 || (byte(7) & 0x80) == 0) {
        return -1;
    }
    return (((byte(9) >> 1) & 0x07) << 30) | (byte(10) << 22) | ((byte(11) >> 1) << 15) |
           (byte(12) << 7) | (byte(13) >> 1);
}

std::uint64_t PtsUnwrapper::unwrap(std::int64_t pts) {
    // The PTS wraps at 2^33. A step back of more than half that range is a wrap.
    // A shorter step back of more than 5 seconds is a discontinuity, as in a
    // looped file; B-frame reordering steps back by far less. The offset then
    // absorbs the step, so the output never goes back.
    constexpr std::int64_t kPtsRange = std::int64_t{1} << 33;
    constexpr std::int64_t kMaxStepBack = 5 * 90000;
    if (m_lastPts >= 0 && pts + kPtsRange / 2 < m_lastPts) {
        m_offset += static_cast<std::uint64_t>(kPtsRange);
    } else if (m_lastPts >= 0 && pts + kMaxStepBack < m_lastPts) {
        qWarning("PTS discontinuity: %lld after %lld (90 kHz); media time continues from the last value.",
                 static_cast<long long>(pts), static_cast<long long>(m_lastPts));
        m_offset += static_cast<std::uint64_t>(m_lastPts - pts);
    }
    m_lastPts = pts;
    return m_offset + static_cast<std::uint64_t>(pts);
}

M2tsPacketizer::M2tsPacketizer(QString sourcePath)
    : m_sourcePath(std::move(sourcePath)),
      m_file(m_sourcePath) {}

void M2tsPacketizer::setTransparent(bool transparent) {
    m_transparent = transparent;
}

QList<int> M2tsPacketizer::retainedSiPids() const {
    return m_retainedSiPids;
}

void M2tsPacketizer::setRetainSiTables(bool retain) {
    m_retainSiTables = retain;
}

void M2tsPacketizer::setRetainNullPackets(bool retain) {
    m_retainNullPackets = retain;
}

bool M2tsPacketizer::open(int requestedProgramNumber, QString* error) {
    m_requestedProgramNumber = requestedProgramNumber;
    if (!m_file.open(QIODevice::ReadOnly)) {
        if (error) {
            *error = QStringLiteral("Failed to open M2TS source: %1").arg(m_file.errorString());
        }
        return false;
    }
    // FIFOs, pipes and /dev/stdin are non-seekable; readObject drains a prebuffer
    // instead of rewinding (see scanPsi).
    m_sequential = m_file.isSequential();
    if (!detectPacketSize(error)) {
        return false;
    }
    // Both modes read the PAT and the PMT of the selected program first. The
    // PCR (video) PID drives the group boundaries, and per-program carriage
    // also needs the PIDs to keep and the initData.
    const bool psiFound = scanPsi(error);
    if (!m_transparent) {
        if (!psiFound || !selectProgramPids(error)) {
            return false;
        }
        // Draft "Mux Rate": a publisher that removes null packets SHOULD
        // declare the rate; for a single-program source it is the nominal mux
        // rate of the source, which the PCR gives.
        if (!m_retainNullPackets && m_patProgramCount == 1) {
            measureMuxRate();
        }
    } else if (!psiFound) {
        // Non-fatal for unmodified carriage: without the PCR PID, the group
        // boundaries latch on the first PID with a random access indicator.
        if (error) {
            *error = QString();
        }
    }
    // Group boundaries follow the random_access_indicator of the video PID, or
    // of the PCR PID when the PMT lists no video stream. The PCR can travel on
    // a PID of its own, which never carries the indicator.
    m_rapPid = m_videoPid >= 0 ? m_videoPid : m_pcrPid;
    // An unmodified-program track carries its source PAT and PMT as initData,
    // so a joining subscriber can pass them to the receiver before the first
    // Object (draft "Use of MSF Initialization Data").
    if (m_transparent && psiFound && m_patProgramCount == 1) {
        refreshInitData();
    }
    m_initDataChanged = false;
    // A live track drops the packets before its first random access point, so
    // that every Group starts at one (draft "Group Boundaries"). A file keeps
    // byte 0. On a multiplex, the points are those of the reference program,
    // so a multiplex without one keeps its lead-in. The random_access_indicator
    // is optional, so the track declares random access only if one appears.
    const bool multiplex = m_transparent && m_patProgramCount != 1;
    m_dropLeadIn = m_sequential && (!multiplex || m_pcrPid >= 0) && findRandomAccess();
    // readObject reads the PSI again from the start, so the assemblers restart.
    // The stored tables stay, so the same tables count as repeats.
    m_patAssembler.reset();
    m_pmtAssembler.reset();
    m_openProgramCount = m_patProgramCount;
    m_opened = true;
    // Non-seekable sources cannot rewind; the prebuffer (or the peeked bytes) is
    // replayed forward-only by readObject.
    return m_sequential ? true : m_file.seek(0);
}

bool M2tsPacketizer::detectPacketSize(QString* error) {
    const QByteArray probe = m_file.peek(192 * 4);
    if (probe.size() < 188) {
        if (error) {
            *error = QStringLiteral("Input is too small to contain a TS packet.");
        }
        return false;
    }

    const auto syncAt = [&probe](int offset) {
        return offset >= 0 && offset < probe.size() && static_cast<unsigned char>(probe[offset]) == 0x47;
    };

    if (syncAt(0) && (probe.size() < 188 * 2 || syncAt(188))) {
        m_packetSize = 188;
        return true;
    }
    if (syncAt(4) && (probe.size() < 192 * 2 || syncAt(196))) {
        m_packetSize = 192;
        return true;
    }

    if (error) {
        *error = QStringLiteral("Input is not packet-aligned TS/M2TS. Expected sync byte at offset 0 for 188-byte TS or offset 4 for 192-byte M2TS.");
    }
    return false;
}

bool M2tsPacketizer::packetHasSync(const QByteArray& packet) const {
    if (packet.size() != m_packetSize) {
        return false;
    }
    const int syncOffset = m_packetSize == 192 ? 4 : 0;
    return static_cast<unsigned char>(packet[syncOffset]) == 0x47;
}

QByteArray M2tsPacketizer::tsPacketView(const QByteArray& sourcePacket) const {
    if (m_packetSize == 192) {
        return sourcePacket.mid(4, 188);
    }
    return sourcePacket;
}

bool M2tsPacketizer::scanPsi(QString* error) {
    const auto fail = [error](const QString& message) {
        if (error) {
            *error = message;
        }
        return false;
    };
    const qint64 originalPos = m_file.pos();
    if (!m_sequential && !m_file.seek(0)) {
        return fail(QStringLiteral("Failed to seek M2TS source while reading the PAT and PMT."));
    }

    constexpr int maxPacketsToScan = 4096;
    for (int index = 0; index < maxPacketsToScan && m_pmt.bytes.isEmpty(); ++index) {
        const QByteArray sourcePacket = m_file.read(m_packetSize);
        if (sourcePacket.size() != m_packetSize) {
            break;
        }
        // On non-seekable input we cannot rewind after the scan, so retain every
        // packet consumed here for readObject to replay in order.
        if (m_sequential) {
            m_prebuffer += sourcePacket;
        }
        if (!packetHasSync(sourcePacket)) {
            return fail(QStringLiteral("Invalid TS sync byte while reading the PAT and PMT."));
        }
        handlePsiPacket(tsPacketView(sourcePacket), sourcePacket);
    }
    // Non-seekable sources keep their forward-only position; readObject drains the
    // prebuffer captured above.
    if (!m_sequential) {
        m_file.seek(originalPos);
    }

    if (m_pmtPid < 0 && m_requestedProgramNumber != 0 && !m_patPrograms.empty()) {
        return fail(QStringLiteral("Requested program %1 was not found in PAT.").arg(m_requestedProgramNumber));
    }
    if (m_pmt.bytes.isEmpty()) {
        return fail(QStringLiteral("Failed to collect PAT/PMT packets for catalog initData."));
    }
    return true;
}

void M2tsPacketizer::handlePsiPacket(const QByteArray& tsPacket, const QByteArray& sourcePacket) {
    const int pid = pidOf(tsPacket);
    if (pid == 0x0000) {
        for (const PsiAssembler::Section& section : m_patAssembler.push(tsPacket, sourcePacket)) {
            onPat(section);
        }
    } else if (pid == m_pmtPid) {
        for (const PsiAssembler::Section& section : m_pmtAssembler.push(tsPacket, sourcePacket)) {
            onPmt(section);
        }
    }
}

void M2tsPacketizer::onPat(const PsiAssembler::Section& section) {
    // A section with current_next_indicator 0 announces a table that does not
    // apply yet (ISO/IEC 13818-1 Section 2.4.4).
    if (section.bytes.size() < 12 || (static_cast<unsigned char>(section.bytes[5]) & 0x01) == 0) {
        return;
    }
    // A large PAT spans several sections. It applies once sections 0 to
    // last_section_number of one version_number are in.
    const int version = (static_cast<unsigned char>(section.bytes[5]) >> 1) & 0x1F;
    const int number = static_cast<unsigned char>(section.bytes[6]);
    const int last = static_cast<unsigned char>(section.bytes[7]);
    if (version != m_patPartsVersion || last != m_patPartsLast) {
        m_patParts.clear();
        m_patPartsVersion = version;
        m_patPartsLast = last;
    }
    if (number > last) {
        return;
    }
    m_patParts[number] = section;
    if (static_cast<int>(m_patParts.size()) != last + 1) {
        return;
    }
    PsiAssembler::Section table;
    std::vector<std::pair<int, int>> programs;
    int networkPid = -1;
    for (const auto& [index, part] : m_patParts) {
        std::vector<std::pair<int, int>> partPrograms;
        int partNetworkPid = -1;
        if (!parsePat(part.bytes, &partPrograms, &partNetworkPid)) {
            return;   // not a PAT
        }
        programs.insert(programs.end(), partPrograms.begin(), partPrograms.end());
        networkPid = partNetworkPid >= 0 ? partNetworkPid : networkPid;
        table.bytes += part.bytes;
        table.sourcePackets += part.sourcePackets;
    }
    if (table.bytes == m_pat.bytes) {
        return;   // a repeat
    }
    m_pat = table;
    m_patPrograms = programs;
    m_networkPid = networkPid;
    m_patProgramCount = static_cast<int>(programs.size());
    if (!m_opened) {
        if (m_pmtPid < 0) {
            // A single-program source has only one program to describe, so the
            // requested number does not apply to it in unmodified carriage.
            const int requested = m_transparent && m_patProgramCount == 1 ? 0 : m_requestedProgramNumber;
            findPatProgram(programs, requested, &m_programNumber, &m_pmtPid);
        }
        return;
    }

    // A PAT change during the session.
    int programNumber = 0;
    int pmtPid = -1;
    const bool listed = findPatProgram(programs, m_programNumber, &programNumber, &pmtPid);
    if (m_transparent && m_openProgramCount == 1 && (m_patProgramCount != 1 || !listed)) {
        // The catalog declares unmodified-program with this program number. A
        // new number needs a new track (draft "Catalog"), which moq2ts cannot
        // add during a session, and two programs make the mode false.
        endTrack(QStringLiteral("The source PAT no longer lists program %1 alone; the "
                                "unmodified-program track ends.").arg(m_programNumber));
        return;
    }
    if (!m_transparent && !listed) {
        // Draft "Per-Program": the publisher SHOULD end the track.
        endTrack(QStringLiteral("Program %1 left the source PAT; the track ends.").arg(m_programNumber));
        return;
    }
    if (!listed && !programs.empty()) {
        // Unmodified-multiplex: the reference program left the PAT. The track
        // goes on, and the catalog's reference fields are now stale advisory
        // values. Group boundaries move to the first program still listed,
        // once its PMT arrives; no PID latches before that.
        qWarning("Reference program %d left the PAT; Groups now follow program %d, and the catalog's "
                 "reference program is stale until the track restarts.",
                 m_programNumber, programs.front().first);
        m_programNumber = programs.front().first;
        m_pmtPid = programs.front().second;
        m_pmtAssembler.reset();
        m_pmt = {};
        m_rapPid = kAwaitingPmt;
    }
    if (listed && pmtPid != m_pmtPid) {
        qWarning("Program %d moved its PMT from PID %d to PID %d.", m_programNumber, m_pmtPid, pmtPid);
        m_pmtPid = pmtPid;
        m_pmtAssembler.reset();
        m_pmt = {};
    }
    if (!m_transparent) {
        rewritePat();
        selectPids();
    }
    refreshInitData();
}

SectionRewriter* M2tsPacketizer::rewriterFor(int pid) {
    if (pid == 0x0001) {
        return &m_catRewriter;
    }
    // With --retain-si, the SDT and the EIT keep only the carried service
    // (draft "Per-Program": SHOULD rewrite the SI). The NIT, TDT, and TOT pass
    // unchanged.
    if (m_retainSiTables && pid == 0x0011) {
        return &m_sdtRewriter;
    }
    if (m_retainSiTables && pid == 0x0012) {
        return &m_eitRewriter;
    }
    return nullptr;
}

SectionRewriter::Decision M2tsPacketizer::rewriteSection(int pid, const QByteArray& section) {
    using Decision = SectionRewriter::Decision;
    if (section.size() < 12) {
        return Decision{};   // shorter than any long-form section with a CRC_32
    }
    const int tableId = static_cast<unsigned char>(section[0]);
    if (pid == 0x0001) {
        return tableId == 0x01 ? rewriteCat(section) : Decision{};
    }
    if (pid == 0x0011) {
        // SDT actual: the carried service only. SDT other describes other
        // transport streams. The BAT passes unchanged.
        if (tableId == 0x42) {
            return rewriteSdt(section);
        }
        return tableId == 0x46 ? Decision{} : Decision{Decision::Keep, {}};
    }
    if (pid == 0x0012) {
        // EIT actual, present/following (0x4E) and schedule (0x50 to 0x5F):
        // the sections of the carried service, unchanged. EIT other (0x4F,
        // 0x60 to 0x6F) describes other transport streams.
        const int serviceId = (static_cast<unsigned char>(section[3]) << 8) | static_cast<unsigned char>(section[4]);
        if (tableId == 0x4E || (tableId >= 0x50 && tableId <= 0x5F)) {
            return serviceId == m_programNumber ? Decision{Decision::Keep, {}} : Decision{};
        }
        return tableId == 0x4F || (tableId >= 0x60 && tableId <= 0x6F) ? Decision{} : Decision{Decision::Keep, {}};
    }
    return Decision{};
}

SectionRewriter::Decision M2tsPacketizer::rewriteSdt(const QByteArray& section) {
    // The service loop starts after original_network_id and a reserved octet.
    // In DVB, the service_id equals the program_number. The section that holds
    // the carried service becomes the only section (section_number and
    // last_section_number 0); the others go.
    const int end = static_cast<int>(section.size()) - 4;
    const int version = (static_cast<unsigned char>(section[5]) >> 1) & 0x1F;
    const int number = static_cast<unsigned char>(section[6]);
    const int last = static_cast<unsigned char>(section[7]);
    for (int offset = 11; offset + 5 <= end;) {
        const int serviceId = (static_cast<unsigned char>(section[offset]) << 8) | static_cast<unsigned char>(section[offset + 1]);
        const int loopLength = ((static_cast<unsigned char>(section[offset + 3]) & 0x0F) << 8) |
                               static_cast<unsigned char>(section[offset + 4]);
        if (offset + 5 + loopLength > end) {
            break;
        }
        if (serviceId == m_programNumber) {
            m_sdtServiceVersion = version;
            QByteArray rewritten = section.left(11) + section.mid(offset, 5 + loopLength) + QByteArray(4, char(0));
            rewritten[6] = char(0x00);   // section_number
            rewritten[7] = char(0x00);   // last_section_number
            const int sectionLength = static_cast<int>(rewritten.size()) - 3;
            rewritten[1] = static_cast<char>((static_cast<unsigned char>(rewritten[1]) & 0xF0) | ((sectionLength >> 8) & 0x0F));
            rewritten[2] = static_cast<char>(sectionLength & 0xFF);
            return SectionRewriter::Decision{SectionRewriter::Decision::Replace, rewritten};
        }
        offset += 5 + loopLength;
    }
    // In DVB the service_id equals the program_number. Say once when no
    // section of an SDT version lists the service, so that the SDT goes.
    if (number == last && m_sdtServiceVersion != version && !m_warnedSdtNoService) {
        m_warnedSdtNoService = true;
        qWarning("An SDT section lists no service %d; it is dropped. In DVB the service_id equals the "
                 "program_number.", m_programNumber);
    }
    return SectionRewriter::Decision{};
}

SectionRewriter::Decision M2tsPacketizer::rewriteCat(const QByteArray& section) {
    // An EMM stream belongs to a CA system, so the entries of the carried
    // program are the CA_descriptors of the CA systems that its ECMs use
    // (draft "Per-Program": SHOULD rewrite the CAT). Other descriptors stay.
    // A program that uses no CA system needs no CAT.
    if (m_caSystems.empty()) {
        if (!m_emmPids.empty()) {
            m_emmPids.clear();
            selectPids();
        }
        return SectionRewriter::Decision{};
    }
    const int end = static_cast<int>(section.size()) - 4;
    QByteArray kept;
    std::set<int> emmPids;
    for (int offset = 8; offset + 2 <= end;) {
        const int tag = static_cast<unsigned char>(section[offset]);
        const int length = static_cast<unsigned char>(section[offset + 1]);
        if (offset + 2 + length > end) {
            break;
        }
        std::set<int> pids;
        std::set<int> systems;
        readCaDescriptors(section, offset, offset + 2 + length, &pids, &systems);
        if (tag != 0x09 || (!systems.empty() && m_caSystems.count(*systems.begin()) != 0)) {
            kept += section.mid(offset, 2 + length);
            emmPids.insert(pids.begin(), pids.end());
        }
        offset += 2 + length;
    }
    // Side effect: the EMM PIDs of the kept descriptors join the PID filter.
    if (emmPids != m_emmPids) {
        m_emmPids = emmPids;
        selectPids();
    }
    QByteArray rewritten = section.left(8) + kept + QByteArray(4, char(0));
    const int sectionLength = static_cast<int>(rewritten.size()) - 3;
    rewritten[1] = static_cast<char>((static_cast<unsigned char>(rewritten[1]) & 0xF0) | ((sectionLength >> 8) & 0x0F));
    rewritten[2] = static_cast<char>(sectionLength & 0xFF);
    return SectionRewriter::Decision{SectionRewriter::Decision::Replace, rewritten};
}

void M2tsPacketizer::endTrack(const QString& reason) {
    m_ended = true;
    m_endReason = reason;
}

void M2tsPacketizer::onPmt(const PsiAssembler::Section& section) {
    // A PMT PID can carry the PMTs of several programs; keep the selected one.
    // A section with current_next_indicator 0 does not apply yet.
    if (section.bytes == m_pmt.bytes || section.bytes.size() < 12 ||
        (static_cast<unsigned char>(section.bytes[5]) & 0x01) == 0 ||
        ((static_cast<unsigned char>(section.bytes[3]) << 8) | static_cast<unsigned char>(section.bytes[4])) !=
            m_programNumber) {
        return;
    }
    int pcrPid = -1;
    int videoPid = -1;
    std::set<int> elementaryPids;
    std::set<int> ecmPids;
    std::set<int> caSystems;
    if (!parsePmt(section.bytes, &pcrPid, &elementaryPids, &videoPid, &ecmPids, &caSystems)) {
        return;
    }
    m_ecmPids = ecmPids;
    m_caSystems = caSystems;
    m_pmt = section;
    m_pcrPid = pcrPid;
    m_videoPid = videoPid;
    m_elementaryPids = elementaryPids;
    if (!m_opened) {
        return;
    }
    // A PMT change during the session. initData follows it, and the pipeline
    // publishes it in a new catalog. The advisory fields keep their values from
    // the start: the draft lets the PSI in the packets take precedence.
    qWarning("The PMT of program %d changed; the track follows it.", m_programNumber);
    m_rapPid = m_videoPid >= 0 ? m_videoPid : m_pcrPid;
    if (!m_transparent) {
        selectPids();
    }
    refreshInitData();
}

void M2tsPacketizer::refreshInitData() {
    // initData is the PAT and the PMT that the track carries: the rewritten PAT
    // in per-program carriage, the source PAT in unmodified-program carriage.
    // A multiplex has none. While a moved PMT has not arrived yet, the tables
    // do not match, so initData waits for it.
    const int programs = m_opened ? m_openProgramCount : m_patProgramCount;
    if ((m_transparent && programs != 1) || m_pat.bytes.isEmpty() || m_pmt.bytes.isEmpty()) {
        return;
    }
    QByteArray initData;
    if (m_transparent) {
        for (const QByteArray& packet : m_pat.sourcePackets) {
            initData += packet;
        }
    } else {
        initData = rewrittenPatPacket(m_pat.sourcePackets.first(), 0);
    }
    for (const QByteArray& packet : m_pmt.sourcePackets) {
        initData += packet;
    }
    if (initData != m_initData) {
        m_initData = initData;
        m_initDataChanged = true;
    }
}

bool M2tsPacketizer::takeInitDataChange(QByteArray* initData) {
    if (!m_initDataChanged) {
        return false;
    }
    m_initDataChanged = false;
    *initData = m_initData;
    return true;
}

void M2tsPacketizer::rewritePat() {
    // Draft "Per-Program": the PAT lists only the program present in this
    // track. The network PID entry stays only with --retain-si, which keeps the
    // NIT. The version_number is set independently of the source, and changes
    // only when the content of the rewritten table changes.
    QByteArray entries;
    const auto appendEntry = [&entries](int program, int pid) {
        entries.append(static_cast<char>((program >> 8) & 0xFF));
        entries.append(static_cast<char>(program & 0xFF));
        entries.append(static_cast<char>(0xE0 | ((pid >> 8) & 0x1F)));
        entries.append(static_cast<char>(pid & 0xFF));
    };
    if (m_retainSiTables && m_networkPid >= 0) {
        appendEntry(0, m_networkPid);
    }
    appendEntry(m_programNumber, m_pmtPid);
    const QByteArray content = m_pat.bytes.mid(3, 2) + entries;   // transport_stream_id and entries
    if (content == m_rewrittenPatContent) {
        return;
    }
    m_rewrittenPatContent = content;
    m_rewrittenPatVersion = (m_rewrittenPatVersion + 1) & 0x1F;   // 0 for the first table

    QByteArray section;
    section.append(char(0x00));                                   // table_id
    const int sectionLength = 5 + static_cast<int>(entries.size()) + 4;
    section.append(static_cast<char>(0xB0 | ((sectionLength >> 8) & 0x0F)));
    section.append(static_cast<char>(sectionLength & 0xFF));
    section.append(content.left(2));
    section.append(static_cast<char>(0xC1 | (m_rewrittenPatVersion << 1)));   // current_next_indicator 1
    section.append(char(0x00));                                   // section_number
    section.append(char(0x00));                                   // last_section_number
    section.append(entries);
    const std::uint32_t crc = mpegCrc32(section.constData(), section.size());
    for (int shift = 24; shift >= 0; shift -= 8) {
        section.append(static_cast<char>((crc >> shift) & 0xFF));
    }
    m_rewrittenPat = section;
}

QByteArray M2tsPacketizer::rewrittenPatPacket(const QByteArray& sourcePacket, int continuityCounter) const {
    QByteArray packet;
    if (m_packetSize == 192) {
        packet += sourcePacket.left(4);   // the prefix of the packet it replaces
    }
    packet.append(char(0x47));
    packet.append(char(0x40));                                    // payload_unit_start_indicator, PID 0
    packet.append(char(0x00));
    packet.append(static_cast<char>(0x10 | (continuityCounter & 0x0F)));   // payload only
    packet.append(char(0x00));                                    // pointer_field
    packet.append(m_rewrittenPat);
    packet.append(QByteArray(m_packetSize - static_cast<int>(packet.size()), char(0xFF)));
    return packet;
}

bool M2tsPacketizer::selectProgramPids(QString* error) {
    // initData carries the rewritten PAT and every packet of the PMT, including
    // a PMT that spans several packets.
    rewritePat();
    refreshInitData();

    if (m_elementaryPids.empty()) {
        if (error) {
            *error = QStringLiteral("Failed to parse selected program PMT elementary PIDs.");
        }
        return false;
    }
    selectPids();
    return true;
}

void M2tsPacketizer::selectPids() {
    m_selectedPids.reset();
    m_selectedPids.set(0x0000);
    m_selectedPids.set(m_pmtPid);
    if (m_pcrPid >= 0) {
        m_selectedPids.set(m_pcrPid);
    }
    for (int pid : m_elementaryPids) {
        m_selectedPids.set(pid);
    }
    // Draft "Per-Program": a publisher filtering a scrambled stream MUST keep
    // the conditional access packets: the CAT, the ECMs that the PMT
    // references, and the EMMs that the CAT references.
    m_selectedPids.set(0x0001);
    for (int pid : m_ecmPids) {
        m_selectedPids.set(pid);
    }
    for (int pid : m_emmPids) {
        m_selectedPids.set(pid);
    }
    // Optional SI-table retention (draft "Per-Program"): keep the well-known
    // DVB PSI/SI PIDs (NIT 0x10, SDT/BAT 0x11, EIT 0x12, TDT/TOT 0x14) alongside
    // the selected program.
    if (m_retainSiTables) {
        m_retainedSiPids.clear();
        for (int siPid : {0x0010, 0x0011, 0x0012, 0x0014}) {
            m_selectedPids.set(siPid);
            m_retainedSiPids.append(siPid);
        }
    }
}

bool M2tsPacketizer::readObject(int packetsPerObject, M2tsObject* object, QString* error) {
    if (object == nullptr || m_packetSize <= 0) {
        if (error) {
            *error = QStringLiteral("Packetizer is not open.");
        }
        return false;
    }

    if (m_ended) {
        if (error) {
            *error = m_endReason;
        }
        return false;
    }

    const int packets = std::max(1, packetsPerObject);
    QByteArray payload;
    payload.reserve(packets * m_packetSize);

    for (int index = 0; index < packets; ++index) {
        // Drain any packets buffered during the init scan (non-seekable sources)
        // before reading further from the device, preserving stream order.
        QByteArray packet;
        if (m_prebufferPos < m_prebuffer.size()) {
            packet = m_prebuffer.mid(m_prebufferPos, m_packetSize);
            m_prebufferPos += m_packetSize;
        } else {
            packet = m_file.read(m_packetSize);
        }
        if (packet.isEmpty()) {
            break;
        }
        if (packet.size() != m_packetSize) {
            if (error) {
                *error = QStringLiteral("Source ended on a partial TS/M2TS packet.");
            }
            return false;
        }
        if (!packetHasSync(packet)) {
            if (error) {
                *error = QStringLiteral("Invalid TS sync byte in source packet.");
            }
            return false;
        }
        // Live PSI tracking, in every mode: the PAT and the PMT of the selected
        // program update the filter, or end the track. The packet that ends the
        // track is not published.
        // A view of the TS packet, without a copy; packet outlives every use.
        const QByteArray tsView = QByteArray::fromRawData(packet.constData() + (m_packetSize == 192 ? 4 : 0), 188);
        const int pid = pidOf(tsView);
        if (pid == 0x0000 || pid == m_pmtPid) {
            handlePsiPacket(tsView, packet);
            if (m_ended) {
                break;
            }
        }
        if (m_dropLeadIn && !m_leadInDropped) {
            if (!startsRandomAccess(pid, tsView)) {
                --index;
                continue;
            }
            m_leadInDropped = true;
        }
        // Unmodified carriage forwards every synced packet (no PID filtering).
        if (!m_transparent) {
            const bool selected = m_selectedPids.test(static_cast<std::size_t>(pid));
            const bool keepNull = m_retainNullPackets && pid == 0x1FFF;
            if (!selected && !keepNull) {
                --index;
                continue;
            }
            // Rewritten tables other than the PAT: their sections go out in new
            // packets, often none, in the place of this one.
            if (SectionRewriter* rewriter = rewriterFor(pid)) {
                const QList<QByteArray> rewritten = rewriter->push(
                    tsView, packet, [this, pid](const QByteArray& section) { return rewriteSection(pid, section); });
                for (const QByteArray& out : rewritten) {
                    payload += out;
                }
                index += static_cast<int>(rewritten.size()) - 1;
                continue;
            }
            // One rewritten PAT packet takes the place of the packet that starts
            // section 0 of each source PAT, which keeps the source repetition
            // rate. The other packets of a long source PAT go. PID 0 gets its
            // own continuity counter.
            if (pid == 0x0000) {
                if (!payloadUnitStart(tsView) || startingSectionNumber(tsView) > 0) {
                    --index;
                    continue;
                }
                packet = rewrittenPatPacket(packet, m_patContinuityCounter);
                m_patContinuityCounter = (m_patContinuityCounter + 1) & 0x0F;
            }
        }
        payload += packet;
    }

    if (payload.isEmpty()) {
        if (m_ended && error) {
            *error = m_endReason;
        }
        return false;
    }

    // One pass over the Object: a random access point starts a new Group, and
    // the first video PES gives the media time (MSF -01 Section 7.1.1).
    bool rapDetected = false;
    object->ptsUs.reset();
    const int ps = m_packetSize;
    const int tsOffset = ps == 192 ? 4 : 0;
    const int ptsPid = m_videoPid >= 0 ? m_videoPid : m_pcrPid;
    for (qsizetype offset = 0; offset + ps <= payload.size(); offset += ps) {
        if (rapDetected && (object->ptsUs.has_value() || ptsPid < 0)) {
            break;
        }
        const QByteArray tsView = QByteArray::fromRawData(payload.constData() + offset + tsOffset, 188);
        const int pid = pidOf(tsView);
        if (!object->ptsUs.has_value() && pid == ptsPid) {
            const std::int64_t pts = pesPts(tsView);
            if (pts >= 0) {
                object->ptsUs = m_ptsUnwrapper.unwrap(pts) * 100 / 9;   // 90 kHz to microseconds, floored
            }
        }
        // Skip PSI (PAT=0x0000, CAT=0x0001) and null (0x1FFF)
        if (rapDetected || pid <= 0x001F || pid == 0x1FFF || !hasRandomAccessIndicator(tsView)) {
            continue;
        }
        // Use the known RAP PID if already identified from PAT/PMT or prior latch.
        if (m_rapPid == -1) {
            m_rapPid = pid; // fallback: latch on first RAI PID seen
        }
        rapDetected = pid == m_rapPid;
    }

    if (rapDetected && m_sawFirstRap) {
        // New group at this RAP boundary
        ++m_currentGroupId;
        m_nextObjectIdInGroup = 0;
    }
    // Draft "Group Boundaries": a Group SHOULD NOT last longer than 2 seconds.
    // Groups follow the random access points of the source, so say once when
    // two of them are further apart.
    if (rapDetected && object->ptsUs.has_value()) {
        if (m_lastGroupPtsUs.has_value() && !m_warnedLongGroup && *object->ptsUs > *m_lastGroupPtsUs + 2000000) {
            m_warnedLongGroup = true;
            qWarning("Random access points %.1f s apart: a Group lasts longer than the 2 s that MSFTS "
                     "recommends. Shorten the GOP at the encoder.",
                     static_cast<double>(*object->ptsUs - *m_lastGroupPtsUs) / 1e6);
        }
        m_lastGroupPtsUs = object->ptsUs;
    }
    if (rapDetected) {
        m_sawFirstRap = true;
    }

    object->payload = std::move(payload);
    object->groupId = m_currentGroupId;
    object->objectId = m_nextObjectIdInGroup++;
    object->startsGroup = rapDetected;
    ++m_nextObjectId;
    return true;
}

QByteArray M2tsPacketizer::lookAheadPacket(qsizetype offset) {
    // A non-seekable source keeps every packet read in the prebuffer, so
    // readObject still publishes it. A seekable one is read in order from its
    // current position.
    if (!m_sequential) {
        return m_file.read(m_packetSize);
    }
    while (offset + m_packetSize > m_prebuffer.size()) {
        const QByteArray packet = m_file.read(m_packetSize);
        if (packet.size() != m_packetSize) {
            return {};
        }
        m_prebuffer += packet;
    }
    return m_prebuffer.mid(offset, m_packetSize);
}

bool M2tsPacketizer::findRandomAccess() {
    // Looks ahead on a live source, up to 20,000 packets (about 2 seconds at
    // 10 Mbit/s), for the first random access point. The packets read stay in
    // the prebuffer, so readObject still publishes them in order.
    constexpr int maxPackets = 20000;
    const int savedRapPid = m_rapPid;
    for (qsizetype offset = 0; offset < qsizetype{maxPackets} * m_packetSize; offset += m_packetSize) {
        const QByteArray packet = lookAheadPacket(offset);
        if (packet.size() != m_packetSize) {
            break;
        }
        const QByteArray tsPacket = tsPacketView(packet);
        if (startsRandomAccess(pidOf(tsPacket), tsPacket)) {
            return true;
        }
    }
    m_rapPid = savedRapPid;
    qWarning("No random_access_indicator within the first %d packets of the live source; the track "
             "starts at its first packet and does not declare mpeg2tsRandomAccess.", maxPackets);
    return false;
}

void M2tsPacketizer::measureMuxRate() {
    // Counts the 188-octet packets between the first PCR and the last PCR read,
    // over at most 20,000 packets or 1 second of PCR time, and divides by the
    // PCR interval. A non-seekable source keeps the packets read in the
    // prebuffer, so readObject still sees them.
    constexpr int maxPackets = 20000;
    constexpr std::int64_t pcrHz = 27000000;
    constexpr std::int64_t pcrRange = (std::int64_t{1} << 33) * 300;
    m_muxRateNote.clear();
    if (m_pcrPid < 0) {
        m_muxRateNote = QStringLiteral("the PMT gives no PCR PID");
        return;
    }
    if (!m_sequential && !m_file.seek(0)) {
        return;
    }
    std::int64_t firstPcr = -1;
    std::int64_t lastPcr = -1;
    std::int64_t elapsed = 0;
    int firstIndex = 0;
    int lastIndex = 0;
    int nullPackets = 0;
    for (int index = 0; index < maxPackets && elapsed < pcrHz; ++index) {
        const QByteArray packet = lookAheadPacket(qsizetype{index} * m_packetSize);
        if (packet.size() != m_packetSize || !packetHasSync(packet)) {
            break;
        }
        const QByteArray tsPacket = tsPacketView(packet);
        const int pid = pidOf(tsPacket);
        nullPackets += pid == 0x1FFF ? 1 : 0;
        const std::int64_t pcr = pid == m_pcrPid ? pcrOf(tsPacket) : -1;
        if (pcr < 0) {
            continue;
        }
        if (firstPcr < 0) {
            firstPcr = pcr;
            firstIndex = index;
        } else {
            elapsed += (pcr - lastPcr + pcrRange) % pcrRange;
        }
        lastPcr = pcr;
        lastIndex = index;
    }
    if (!m_sequential) {
        m_file.seek(0);
    }
    if (elapsed <= 0 || lastIndex <= firstIndex) {
        m_muxRateNote = QStringLiteral("fewer than two PCRs at the start of the source");
    } else if (nullPackets == 0) {
        // Without null stuffing the source is likely VBR, and the measure would
        // be an average, not a nominal rate.
        m_muxRateNote = QStringLiteral("the source carries no null packets, so it is likely VBR");
    } else {
        m_measuredMuxRate = (static_cast<std::int64_t>(lastIndex - firstIndex) * 188 * 8 * pcrHz + elapsed / 2) / elapsed;
    }
}

qint64 M2tsPacketizer::measuredMuxRate() const {
    return m_measuredMuxRate;
}

QString M2tsPacketizer::muxRateNote() const {
    return m_muxRateNote;
}

bool M2tsPacketizer::startsRandomAccess(int pid, const QByteArray& tsPacket) {
    if (pid <= 0x001F || pid == 0x1FFF || !hasRandomAccessIndicator(tsPacket)) {
        return false;
    }
    if (m_rapPid == -1) {
        m_rapPid = pid;   // fallback: latch on the first PID with the indicator
    }
    return pid == m_rapPid;
}

bool M2tsPacketizer::randomAccess() const {
    return m_dropLeadIn;
}

bool M2tsPacketizer::hasRandomAccessIndicator(const QByteArray& tsPacket) const {
    // TS packet adaptation field: byte 3 bits 5-4 = adaptation_field_control.
    // Values 2 (AF only) or 3 (AF + payload) indicate an adaptation field is present.
    // The adaptation field flags byte (byte 5) bit 6 = random_access_indicator.
    if (tsPacket.size() < 6) {
        return false;
    }
    const int adaptationControl = (static_cast<unsigned char>(tsPacket[3]) >> 4) & 0x03;
    if (adaptationControl < 2) {
        return false; // no adaptation field
    }
    const int afLength = static_cast<unsigned char>(tsPacket[4]);
    if (afLength < 1) {
        return false; // no flags byte
    }
    // Bit 6 of the flags byte is random_access_indicator
    return (static_cast<unsigned char>(tsPacket[5]) & 0x40) != 0;
}

int M2tsPacketizer::packetSize() const {
    return m_packetSize;
}

int M2tsPacketizer::programNumber() const {
    return m_programNumber;
}

int M2tsPacketizer::pmtPid() const {
    return m_pmtPid;
}

int M2tsPacketizer::pcrPid() const {
    return m_pcrPid;
}

int M2tsPacketizer::patProgramCount() const {
    return m_patProgramCount;
}

QByteArray M2tsPacketizer::initData() const {
    return m_initData;
}

std::uint64_t M2tsPacketizer::objectsRead() const {
    return m_nextObjectId;
}

bool M2tsPacketizer::sequential() const {
    return m_sequential;
}

qint64 M2tsPacketizer::probeDurationMs(const QString& sourcePath) {
#ifdef MOQ2TS_HAVE_LIBAV_CAPTURE
    if (sourcePath.isEmpty()) {
        return 0;
    }
    AVFormatContext* ctx = nullptr;
    const QByteArray path = sourcePath.toUtf8();
    if (avformat_open_input(&ctx, path.constData(), nullptr, nullptr) != 0) {
        return 0;
    }
    qint64 durationMs = 0;
    if (avformat_find_stream_info(ctx, nullptr) >= 0 && ctx->duration > 0) {
        // AVFormatContext::duration is in AV_TIME_BASE units (microseconds).
        durationMs = static_cast<qint64>((ctx->duration + (AV_TIME_BASE / 2000)) / (AV_TIME_BASE / 1000));
    }
    avformat_close_input(&ctx);
    return durationMs;
#else
    Q_UNUSED(sourcePath);
    return 0;
#endif
}

} // namespace moq2ts
