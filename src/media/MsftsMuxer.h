#pragma once

#include <QByteArray>
#include <QList>
#include <QString>

#include <cstdint>

namespace moq2ts {

// The values of mpeg2tsMode (draft-gregoire-moq-msfts) whose Objects carry the
// TS packets of a program or a multiplex. The fourth value, es-packets, needs
// mpeg2tsEsPid, which this encoder does not produce; the parser skips it.
enum class Mpeg2tsMode {
    UnmodifiedProgram,
    UnmodifiedMultiplex,
    PerProgram,
};

struct MsftsCatalog {
    QString track;
    int packetSize = 188;
    // Encoder-only knob (source packets per Object); not a catalog field.
    int packetsPerObject = 7;
    // The carried program, or in unmodified-multiplex the reference program,
    // named in the catalog only when pcrPid >= 0.
    int programNumber = 1;
    int pcrPid = -1;
    // draft-gregoire-moq-msfts mpeg2tsSiPids: SI table PIDs retained in the
    // filtered track in addition to those listed in the PMT. Advisory; emitted
    // only when non-empty, which is only when SI retention is on and the track
    // is filtered.
    QList<int> siPids;
    // Per draft-gregoire-moq-msfts, mpeg2tsTimestampMode applies only to
    // 192-octet source packets ("arrival-time" or "opaque") and MUST NOT be
    // present for 188.
    QString timestampMode;
    QByteArray initData;
    QString timelineTrack;

    // Root "version" identifies the referenced MSF revision, not a version of the
    // MSFTS packaging format (draft-gregoire-moq-msfts). Every catalog example
    // in that draft carries it as the string "draft-01".
    QString msfVersion = QStringLiteral("draft-01");
    // MSF track namespace (per-track, MSF 5.1.10). Emitted only when non-empty.
    QString namespaceName;
    // VOD-only track duration in integer milliseconds (MSF 5.1.37); emitted only
    // when isLive is false and the value is > 0.
    qint64 trackDurationMs = 0;
    // When true, advertise mpeg2tsRandomAccess (draft-gregoire-moq-msfts): the
    // first Object of every MOQT Group contains a random access point.
    bool randomAccess = false;

    // Advertised as mpeg2tsMode (draft-gregoire-moq-msfts). The two unmodified
    // modes carry the source verbatim: UnmodifiedProgram when its PAT lists one
    // program, UnmodifiedMultiplex otherwise. PerProgram is a program derived by
    // filtering.
    Mpeg2tsMode mode = Mpeg2tsMode::PerProgram;
    // Advisory stuffing target in bits/s. Emitted as mpeg2tsMuxRate only when
    // > 0 and the mode is PerProgram.
    qint64 mpeg2tsMuxRateBps = 0;

    // MSF common track/root fields (draft-ietf-moq-msf-01).
    bool isLive = true;
    int targetLatencyMs = 1000;
    // MSF 5.2.6: the content the track carries. Empty omits the field, as for a
    // multiplex, which no single role describes.
    QString role = QStringLiteral("video");
    QString mimeType = QStringLiteral("video/mp2t");
    qint64 bitrateBps = 0;
    qint64 generatedAtMs = 0;
    // MSF accessibleWindowTime (editor's copy): the time in ms behind the live
    // edge during which content stays accessible. Emitted on a live media track
    // when > 0.
    qint64 accessibleWindowMs = 0;
};

// The mpeg2tsMuxRate (MSFTS 6.7) for a program measured at measuredBps: 10 %
// above it, to cover the jitter of a fixed-rate service over the one-second
// measure, rounded up to a multiple of 10,000 bit/s. 0 when measuredBps <= 0.
qint64 workedOutMuxRate(qint64 measuredBps);

class MsftsMuxer {
public:
    static QByteArray catalogJson(const MsftsCatalog& catalog);
    // One catalog listing every entry: each contributes its media track, its
    // timeline track (when named), and its initDataList entry (when it has
    // initData), in list order. Root fields (version, generatedAt) come from
    // the first entry. An empty list yields an empty "tracks" array and no
    // initDataList.
    static QByteArray catalogJson(const QList<MsftsCatalog>& catalogs);
    // The independent catalog that ends a live broadcast for good (MSF -01
    // Section 11.3): isComplete true and no track.
    static QByteArray completeCatalogJson(const MsftsCatalog& catalog);

    // One MSF media timeline record (draft-ietf-moq-msf-01 Section 7.1.1):
    //   [mediaPresentationTimeMs, [groupId, objectId], wallclockMs]
    // Both times are the floor in integral milliseconds. wallclockUnixUs is 0
    // when the wallclock time is unknown, as for a VOD asset.
    static QByteArray mediaTimelineRecord(std::uint64_t mediaTimeUs,
                                          std::uint64_t groupId,
                                          std::uint64_t objectId,
                                          std::uint64_t wallclockUnixUs);

    // Inverse of catalogJson: parse an MSFTS catalog document and fill the
    // mpeg2ts media-track fields a receiver needs (packetSize, mode,
    // program/pcr, muxRate, randomAccess, timestampMode, isLive, decoded
    // initData). The media track's name is returned via mediaTrackName. Returns
    // false (and sets error) when the document is invalid or has no mpeg2ts
    // track.
    static bool catalogFromJson(const QByteArray& json,
                                MsftsCatalog* out,
                                QString* mediaTrackName,
                                QString* error);
};

} // namespace moq2ts
