#pragma once

// Builds synthetic 188-octet TS packets for unit tests: PAT and PMT sections
// with valid CRC_32 values, and video PES packets with a PTS.

#include <QByteArray>
#include <QList>

#include <cstdint>
#include <cstdio>
#include <algorithm>
#include <cstdlib>
#include <utility>

namespace moq2ts::test {

// MPEG-2 CRC_32 (ISO/IEC 13818-1 Annex A): polynomial 0x04C11DB7, initial value
// 0xFFFFFFFF, no reflection, no final XOR.
inline std::uint32_t mpegCrc32(const QByteArray& data) {
    std::uint32_t crc = 0xFFFFFFFFu;
    for (const char byte : data) {
        crc ^= static_cast<std::uint32_t>(static_cast<unsigned char>(byte)) << 24;
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc & 0x80000000u) ? (crc << 1) ^ 0x04C11DB7u : crc << 1;
        }
    }
    return crc;
}

inline void appendU16(QByteArray* out, int value) {
    out->append(static_cast<char>((value >> 8) & 0xFF));
    out->append(static_cast<char>(value & 0xFF));
}

// Completes a section: fills section_length and appends the CRC_32.
inline QByteArray finishSection(QByteArray section) {
    const int sectionLength = section.size() - 3 + 4;
    section[1] = static_cast<char>((static_cast<unsigned char>(section[1]) & 0xF0) | ((sectionLength >> 8) & 0x0F));
    section[2] = static_cast<char>(sectionLength & 0xFF);
    const std::uint32_t crc = mpegCrc32(section);
    for (int shift = 24; shift >= 0; shift -= 8) {
        section.append(static_cast<char>((crc >> shift) & 0xFF));
    }
    return section;
}

// PAT listing (program_number, PMT PID) pairs.
inline QByteArray patSection(const QList<std::pair<int, int>>& programs, int version = 0, int sectionNumber = 0,
                             int lastSectionNumber = 0, bool currentNext = true) {
    QByteArray section;
    section.append(char(0x00));          // table_id
    section.append(char(0xB0));          // section_syntax_indicator, length filled later
    section.append(char(0x00));
    appendU16(&section, 1);              // transport_stream_id
    section.append(static_cast<char>(0xC0 | ((version & 0x1F) << 1) | (currentNext ? 0x01 : 0x00)));
    section.append(static_cast<char>(sectionNumber));
    section.append(static_cast<char>(lastSectionNumber));
    for (const auto& [program, pmtPid] : programs) {
        appendU16(&section, program);
        appendU16(&section, 0xE000 | pmtPid);
    }
    return finishSection(section);
}

// PMT of one program: (stream_type, elementary PID) pairs.
inline QByteArray pmtSection(int programNumber, int pcrPid, const QList<std::pair<int, int>>& streams,
                             int version = 0) {
    QByteArray section;
    section.append(char(0x02));          // table_id
    section.append(char(0xB0));
    section.append(char(0x00));
    appendU16(&section, programNumber);
    section.append(static_cast<char>(0xC1 | ((version & 0x1F) << 1)));
    section.append(char(0x00));
    section.append(char(0x00));
    appendU16(&section, 0xE000 | pcrPid);
    appendU16(&section, 0xF000);         // program_info_length 0
    for (const auto& [streamType, pid] : streams) {
        section.append(static_cast<char>(streamType));
        appendU16(&section, 0xE000 | pid);
        appendU16(&section, 0xF000);     // ES_info_length 0
    }
    return finishSection(section);
}

// A CA_descriptor (tag 0x09): CA_system_id and CA_PID.
inline QByteArray caDescriptor(int caSystemId, int caPid) {
    QByteArray descriptor;
    descriptor.append(char(0x09));
    descriptor.append(char(0x04));
    appendU16(&descriptor, caSystemId);
    appendU16(&descriptor, 0xE000 | caPid);
    return descriptor;
}

// A PMT with descriptors: programInfo for the program loop, and per stream
// (stream_type, PID, ES_info descriptors).
struct PmtStream {
    int streamType;
    int pid;
    QByteArray esInfo;
};

inline QByteArray pmtSectionWithDescriptors(int programNumber, int pcrPid, const QByteArray& programInfo,
                                            const QList<PmtStream>& streams, int version = 0) {
    QByteArray section;
    section.append(char(0x02));
    section.append(char(0xB0));
    section.append(char(0x00));
    appendU16(&section, programNumber);
    section.append(static_cast<char>(0xC1 | ((version & 0x1F) << 1)));
    section.append(char(0x00));
    section.append(char(0x00));
    appendU16(&section, 0xE000 | pcrPid);
    appendU16(&section, 0xF000 | static_cast<int>(programInfo.size()));
    section.append(programInfo);
    for (const PmtStream& stream : streams) {
        section.append(static_cast<char>(stream.streamType));
        appendU16(&section, 0xE000 | stream.pid);
        appendU16(&section, 0xF000 | static_cast<int>(stream.esInfo.size()));
        section.append(stream.esInfo);
    }
    return finishSection(section);
}

// Any long-form section: table_id, table_id_extension, the body after
// last_section_number, a version, and section numbers.
inline QByteArray longSection(int tableId, int extension, const QByteArray& body, int version = 0,
                              int sectionNumber = 0, int lastSectionNumber = 0) {
    QByteArray section;
    section.append(static_cast<char>(tableId));
    section.append(char(0xF0));          // section_syntax_indicator, reserved_future_use
    section.append(char(0x00));
    appendU16(&section, extension);
    section.append(static_cast<char>(0xC1 | ((version & 0x1F) << 1)));
    section.append(static_cast<char>(sectionNumber));
    section.append(static_cast<char>(lastSectionNumber));
    section.append(body);
    return finishSection(section);
}

// An SDT body: original_network_id, a reserved octet, and per service
// (service_id, descriptors).
inline QByteArray sdtBody(int originalNetworkId, const QList<std::pair<int, QByteArray>>& services) {
    QByteArray body;
    appendU16(&body, originalNetworkId);
    body.append(char(0xFF));
    for (const auto& [serviceId, descriptors] : services) {
        appendU16(&body, serviceId);
        body.append(char(0xFC));         // reserved, no EIT flags
        appendU16(&body, 0x8000 | static_cast<int>(descriptors.size()));   // running
        body.append(descriptors);
    }
    return body;
}

// A CAT (table_id 0x01) with the given descriptors.
inline QByteArray catSection(const QByteArray& descriptors, int version = 0) {
    QByteArray section;
    section.append(char(0x01));
    section.append(char(0xB0));
    section.append(char(0x00));
    appendU16(&section, 0xFFFF);         // reserved table_id_extension
    section.append(static_cast<char>(0xC1 | ((version & 0x1F) << 1)));
    section.append(char(0x00));
    section.append(char(0x00));
    section.append(descriptors);
    return finishSection(section);
}

// One TS packet on pid. When adaptation is set, the packet carries an adaptation
// field with the given flags byte (0x40 = random_access_indicator). The payload
// is padded with 0xFF stuffing.
inline QByteArray tsPacket(int pid, bool payloadUnitStart, const QByteArray& payload,
                           int continuityCounter = 0, int adaptationFlags = -1) {
    QByteArray packet;
    packet.append(char(0x47));
    packet.append(static_cast<char>((payloadUnitStart ? 0x40 : 0x00) | ((pid >> 8) & 0x1F)));
    packet.append(static_cast<char>(pid & 0xFF));
    const bool adaptation = adaptationFlags >= 0;
    // Q_ASSERT is compiled out in release builds, so a bad test input aborts here.
    if (payload.size() > (adaptation ? 182 : 184)) {
        std::fprintf(stderr, "tsPacket: payload of %lld octets does not fit\n", static_cast<long long>(payload.size()));
        std::abort();
    }
    packet.append(static_cast<char>((adaptation ? 0x30 : 0x10) | (continuityCounter & 0x0F)));
    if (adaptation) {
        const int stuffing = 188 - 4 - 2 - payload.size();
        packet.append(static_cast<char>(1 + stuffing));   // adaptation_field_length
        packet.append(static_cast<char>(adaptationFlags));
        packet.append(QByteArray(stuffing, char(0xFF)));
        packet.append(payload);
    } else {
        packet.append(payload);
        packet.append(QByteArray(188 - packet.size(), char(0xFF)));
    }
    return packet;
}

// The PID of the first TS packet of a payload of 188-octet packets.
inline int pidOfFirst(const QByteArray& payload) {
    return payload.size() < 3 ? -1
                              : ((static_cast<unsigned char>(payload[1]) & 0x1F) << 8) | static_cast<unsigned char>(payload[2]);
}

// A PSI packet: pointer_field 0 followed by the section.
inline QByteArray psiPacket(int pid, const QByteArray& section, int continuityCounter = 0) {
    QByteArray payload;
    payload.append(char(0x00));
    payload.append(section);
    return tsPacket(pid, true, payload, continuityCounter);
}

// A section split over as many packets as it needs: pointer_field 0 in the
// first packet, then continuation packets, padded with 0xFF.
inline QList<QByteArray> psiPackets(int pid, const QByteArray& section, int firstContinuityCounter = 0) {
    QList<QByteArray> packets;
    QByteArray rest = section;
    int cc = firstContinuityCounter;
    bool first = true;
    while (first || !rest.isEmpty()) {
        QByteArray payload;
        if (first) {
            payload.append(char(0x00));
        }
        const int room = 184 - static_cast<int>(payload.size());
        payload.append(rest.left(room));
        rest.remove(0, std::min<qsizetype>(room, rest.size()));
        packets.append(tsPacket(pid, first, payload, cc));
        cc = (cc + 1) & 0x0F;
        first = false;
    }
    return packets;
}

// The start of a video PES packet with a PTS (33 bits, 90 kHz).
inline QByteArray pesHeaderWithPts(std::uint64_t pts) {
    QByteArray pes;
    pes.append(char(0x00));
    pes.append(char(0x00));
    pes.append(char(0x01));
    pes.append(char(0xE0));                  // stream_id: video
    pes.append(char(0x00));
    pes.append(char(0x00));                  // PES_packet_length 0 (unbounded)
    pes.append(char(0x80));                  // marker bits
    pes.append(char(0x80));                  // PTS_DTS_flags = '10'
    pes.append(char(0x05));                  // PES_header_data_length
    pes.append(static_cast<char>(0x21 | ((pts >> 29) & 0x0E)));
    pes.append(static_cast<char>((pts >> 22) & 0xFF));
    pes.append(static_cast<char>(0x01 | ((pts >> 14) & 0xFE)));
    pes.append(static_cast<char>((pts >> 7) & 0xFF));
    pes.append(static_cast<char>(0x01 | ((pts << 1) & 0xFE)));
    pes.append(QByteArray(8, char(0x00)));   // start of the elementary stream
    return pes;
}

} // namespace moq2ts::test
