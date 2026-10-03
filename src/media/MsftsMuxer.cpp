#include "MsftsMuxer.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QHash>

namespace moq2ts {

namespace {

QString modeName(Mpeg2tsMode mode) {
    switch (mode) {
    case Mpeg2tsMode::UnmodifiedProgram:
        return QStringLiteral("unmodified-program");
    case Mpeg2tsMode::UnmodifiedMultiplex:
        return QStringLiteral("unmodified-multiplex");
    case Mpeg2tsMode::PerProgram:
        break;
    }
    return QStringLiteral("per-program");
}

} // namespace

QByteArray MsftsMuxer::catalogJson(const MsftsCatalog& catalog) {
    QJsonObject mediaTrack;
    mediaTrack.insert(QStringLiteral("name"), catalog.track);
    if (!catalog.namespaceName.isEmpty()) {
        mediaTrack.insert(QStringLiteral("namespace"), catalog.namespaceName);
    }
    mediaTrack.insert(QStringLiteral("packaging"), QStringLiteral("mpeg2ts"));
    // MSF common track fields (draft-ietf-moq-msf-01).
    mediaTrack.insert(QStringLiteral("isLive"), catalog.isLive);
    mediaTrack.insert(QStringLiteral("role"), catalog.role);
    mediaTrack.insert(QStringLiteral("mimeType"), catalog.mimeType);
    if (catalog.isLive) {
        // targetLatency MUST NOT be present when isLive is false (MSF 5.1.16).
        mediaTrack.insert(QStringLiteral("targetLatency"), catalog.targetLatencyMs);
    }
    if (catalog.bitrateBps > 0) {
        mediaTrack.insert(QStringLiteral("bitrate"), catalog.bitrateBps);
    }
    // MSFTS mpeg2ts-specific fields (draft-gregoire-moq-msfts).
    mediaTrack.insert(QStringLiteral("mpeg2tsPacketSize"), catalog.packetSize);
    // mpeg2tsMode is required (draft-gregoire-moq-msfts).
    mediaTrack.insert(QStringLiteral("mpeg2tsMode"), modeName(catalog.mode));
    // In unmodified-multiplex the publisher MAY name a reference program, on
    // whose PCR a subscriber paces the multiplex. The track names it only when
    // its PCR PID is known. mpeg2tsMuxRate and mpeg2tsSiPids stay forbidden in
    // that mode.
    const bool multiplex = catalog.mode == Mpeg2tsMode::UnmodifiedMultiplex;
    const bool namesProgram = !multiplex || catalog.pcrPid >= 0;
    if (namesProgram) {
        mediaTrack.insert(QStringLiteral("mpeg2tsProgramNumber"), catalog.programNumber);
        if (catalog.pcrPid >= 0) {
            mediaTrack.insert(QStringLiteral("mpeg2tsPcrPid"), catalog.pcrPid);
        }
    }
    if (!multiplex) {
        // Advisory source constant mux rate.
        if (catalog.mpeg2tsMuxRateBps > 0) {
            mediaTrack.insert(QStringLiteral("mpeg2tsMuxRate"), catalog.mpeg2tsMuxRateBps);
        }
    }
    // mpeg2tsSiPids: PIDs of SI tables that a per-program track retains beyond
    // those the PMT lists. Advisory.
    if (catalog.mode == Mpeg2tsMode::PerProgram && !catalog.siPids.isEmpty()) {
        QJsonArray siPids;
        for (int pid : catalog.siPids) {
            siPids.append(pid);
        }
        mediaTrack.insert(QStringLiteral("mpeg2tsSiPids"), siPids);
    }
    // mpeg2tsTimestampMode is only valid for 192-octet source packets
    // (draft-gregoire-moq-msfts); it MUST NOT be present for 188.
    if (catalog.packetSize == 192 && !catalog.timestampMode.isEmpty()) {
        mediaTrack.insert(QStringLiteral("mpeg2tsTimestampMode"), catalog.timestampMode);
    }
    // Only advertised when the first Object of every Group contains a random
    // access point. On a multiplex, that means the reference program's points,
    // so the field MUST be absent without a reference program.
    if (catalog.randomAccess && namesProgram) {
        mediaTrack.insert(QStringLiteral("mpeg2tsRandomAccess"), true);
    }
    // MSF 5.1.37: track duration is VOD-only (MUST NOT appear when isLive true).
    if (!catalog.isLive && catalog.trackDurationMs > 0) {
        mediaTrack.insert(QStringLiteral("trackDuration"), catalog.trackDurationMs);
    }
    // Initialization data is referenced, not inlined on the track. MSF-01 replaced
    // the old track-level initData field (MSF-00 5.1.20) with initRef (5.2.13)
    // pointing into a root initDataList (5.1.7), and MSFTS requires the
    // referenced entry's type to be "inline". Emitting the MSF-00 shape would be
    // silently dropped by a conformant receiver, since MSFTS tells parsers to
    // ignore fields they do not understand, leaving a filtered track with no PSI
    // bootstrap at all.
    const QString initRefId = QStringLiteral("init-") + catalog.track;
    if (!catalog.initData.isEmpty()) {
        mediaTrack.insert(QStringLiteral("initRef"), initRefId);
    }

    QJsonArray tracks;
    tracks.append(mediaTrack);

    if (!catalog.timelineTrack.isEmpty()) {
        // MSF media timeline track (draft-ietf-moq-msf-01 Section 7.2): identified
        // by packaging "mediatimeline" (Table 3), a "depends" list of the track
        // names it applies to, and an application/json mime type. Section 7.2
        // says 'type', but MSF defines no such field (moq-wg/msf#213).
        QJsonObject timelineTrack;
        timelineTrack.insert(QStringLiteral("name"), catalog.timelineTrack);
        if (!catalog.namespaceName.isEmpty()) {
            timelineTrack.insert(QStringLiteral("namespace"), catalog.namespaceName);
        }
        timelineTrack.insert(QStringLiteral("packaging"), QStringLiteral("mediatimeline"));
        QJsonArray depends;
        depends.append(catalog.track);
        timelineTrack.insert(QStringLiteral("depends"), depends);
        timelineTrack.insert(QStringLiteral("mimeType"), QStringLiteral("application/json"));

        tracks.append(timelineTrack);
    }

    QJsonObject root;
    // MSF-01 5.1.1 makes version a String and says to write "draft-XX" against
    // Internet-Draft releases; MSFTS 2 requires the value specified by the MSF
    // revision it references, which is draft-ietf-moq-msf-01. No "format" field is
    // emitted: it exists in neither document.
    root.insert(QStringLiteral("version"), catalog.msfVersion);
    if (catalog.isLive && catalog.generatedAtMs > 0) {
        // SHOULD NOT be included when isLive is false (MSF 5.1.6).
        root.insert(QStringLiteral("generatedAt"), catalog.generatedAtMs);
    }
    root.insert(QStringLiteral("tracks"), tracks);
    QByteArray json = QJsonDocument(root).toJson(QJsonDocument::Compact);
    if (!catalog.initData.isEmpty()) {
        // MSF 5.1.7: each entry is {id, type, data}; MSFTS fixes type to
        // "inline" and requires the decoded data to be whole source packets, which
        // it is because collectInitData() only ever captures complete TS packets.
        QJsonObject initEntry;
        initEntry.insert(QStringLiteral("id"), initRefId);
        initEntry.insert(QStringLiteral("type"), QStringLiteral("inline"));
        initEntry.insert(QStringLiteral("data"), QString::fromLatin1(catalog.initData.toBase64()));
        QJsonArray initDataList;
        initDataList.append(initEntry);
        // MSF 5.1.7: the list MUST be located after the tracks array.
        // QJsonObject keeps its keys sorted, which would put "initDataList"
        // first, so the list is appended to the serialized root instead.
        Q_ASSERT(json.endsWith('}'));
        json.chop(1);
        json += ",\"initDataList\":" + QJsonDocument(initDataList).toJson(QJsonDocument::Compact) + '}';
    }
    return json;
}

QByteArray MsftsMuxer::mediaTimelineRecord(std::uint64_t mediaTimeUs,
                                           std::uint64_t groupId,
                                           std::uint64_t objectId,
                                           std::uint64_t wallclockUnixUs) {
    QJsonArray location;
    location.append(static_cast<qint64>(groupId));
    location.append(static_cast<qint64>(objectId));

    QJsonArray record;
    record.append(static_cast<qint64>(mediaTimeUs / 1000ULL));
    record.append(location);
    record.append(static_cast<qint64>(wallclockUnixUs / 1000ULL));
    return QJsonDocument(record).toJson(QJsonDocument::Compact);
}

bool MsftsMuxer::catalogFromJson(const QByteArray& json,
                                 MsftsCatalog* out,
                                 QString* mediaTrackName,
                                 QString* error) {
    const auto fail = [&](const QString& message) {
        if (error) {
            *error = message;
        }
        return false;
    };
    if (out == nullptr) {
        return fail(QStringLiteral("catalogFromJson: null output."));
    }

    QJsonParseError parseError{};
    const QJsonDocument doc = QJsonDocument::fromJson(json, &parseError);
    if (doc.isNull() || !doc.isObject()) {
        return fail(QStringLiteral("Catalog is not a JSON object: %1").arg(parseError.errorString()));
    }
    const QJsonObject root = doc.object();
    // "version" names the referenced MSF revision. The MSFTS draft writes it as a
    // string ("draft-01"); older catalogs of ours wrote the integer 1. Accept both
    // so a publisher upgrade does not strand receivers mid-rollout.
    const QJsonValue versionValue = root.value(QStringLiteral("version"));
    QString msfVersion;
    if (versionValue.isString()) {
        msfVersion = versionValue.toString();
    } else if (versionValue.isDouble()) {
        msfVersion = QString::number(versionValue.toInt());
    }
    // MSF 5.1.7 initDataList, keyed by id so a track's initRef (5.2.13) can be
    // resolved. Only "inline" entries are usable: MSFTS defines no other type,
    // and an unknown type carries data this code cannot interpret.
    QHash<QString, QByteArray> initDataById;
    for (const QJsonValue& entryValue : root.value(QStringLiteral("initDataList")).toArray()) {
        const QJsonObject entry = entryValue.toObject();
        if (entry.value(QStringLiteral("type")).toString() != QStringLiteral("inline")) {
            continue;
        }
        const QString id = entry.value(QStringLiteral("id")).toString();
        if (id.isEmpty()) {
            continue;
        }
        initDataById.insert(id, QByteArray::fromBase64(
            entry.value(QStringLiteral("data")).toString().toLatin1()));
    }

    const QJsonArray tracks = root.value(QStringLiteral("tracks")).toArray();
    if (tracks.isEmpty()) {
        return fail(QStringLiteral("Catalog has no tracks."));
    }

    QString unsupportedMode;
    for (const QJsonValue value : tracks) {
        const QJsonObject track = value.toObject();
        // The draft names this value "mpeg2ts"; accept the legacy "m2ts"
        // spelling as a fallback so catalogs from older publishers still parse.
        const QString packaging = track.value(QStringLiteral("packaging")).toString();
        if (packaging != QStringLiteral("mpeg2ts") && packaging != QStringLiteral("m2ts")) {
            continue;
        }

        MsftsCatalog parsed;
        parsed.track = track.value(QStringLiteral("name")).toString();
        parsed.namespaceName = track.value(QStringLiteral("namespace")).toString();
        parsed.packetSize = track.value(QStringLiteral("mpeg2tsPacketSize"))
                                 .toInt(track.value(QStringLiteral("m2tsPacketSize")).toInt(188));
        // The draft names this field mpeg2tsMode. This parser reads the three
        // modes whose Objects carry the TS packets of a program or a multiplex;
        // accept the legacy m2tsMpts/m2tsTransparent booleans as a fallback so
        // catalogs from older publishers still parse.
        // An es-packets track carries one PID only and cannot be read as a
        // program; skip such tracks, and any unknown mode, instead.
        const QString mode = track.value(QStringLiteral("mpeg2tsMode")).toString();
        if (mode == QStringLiteral("unmodified-program")) {
            parsed.mode = Mpeg2tsMode::UnmodifiedProgram;
        } else if (mode == QStringLiteral("unmodified-multiplex")) {
            parsed.mode = Mpeg2tsMode::UnmodifiedMultiplex;
        } else if (mode == QStringLiteral("per-program")) {
            parsed.mode = Mpeg2tsMode::PerProgram;
        } else if (!mode.isEmpty()) {
            unsupportedMode = mode;
            continue;
        } else {
            const bool legacyWholeMultiplex = track.value(QStringLiteral("m2tsMpts"))
                .toBool(track.value(QStringLiteral("m2tsTransparent")).toBool(false));
            parsed.mode = legacyWholeMultiplex ? Mpeg2tsMode::UnmodifiedMultiplex
                                               : Mpeg2tsMode::PerProgram;
        }
        parsed.programNumber = track.value(QStringLiteral("mpeg2tsProgramNumber"))
                                    .toInt(track.value(QStringLiteral("m2tsProgramNumber")).toInt(0));
        parsed.pcrPid = track.contains(QStringLiteral("mpeg2tsPcrPid"))
                            ? track.value(QStringLiteral("mpeg2tsPcrPid")).toInt(-1)
                        : track.contains(QStringLiteral("m2tsPcrPid"))
                            ? track.value(QStringLiteral("m2tsPcrPid")).toInt(-1)
                            : -1;
        parsed.mpeg2tsMuxRateBps = static_cast<qint64>(
            track.value(QStringLiteral("mpeg2tsMuxRate"))
                .toDouble(track.value(QStringLiteral("m2tsMuxRate")).toDouble(0.0)));
        parsed.siPids.clear();
        const QJsonArray siPidsArray = track.contains(QStringLiteral("mpeg2tsSiPids"))
            ? track.value(QStringLiteral("mpeg2tsSiPids")).toArray()
            : track.value(QStringLiteral("m2tsSiPids")).toArray();
        for (const QJsonValue& pid : siPidsArray) {
            parsed.siPids.append(pid.toInt());
        }
        parsed.timestampMode = track.contains(QStringLiteral("mpeg2tsTimestampMode"))
            ? track.value(QStringLiteral("mpeg2tsTimestampMode")).toString()
            : track.value(QStringLiteral("m2tsTimestampMode")).toString();
        parsed.randomAccess = track.value(QStringLiteral("mpeg2tsRandomAccess"))
                                   .toBool(track.value(QStringLiteral("m2tsRandomAccess")).toBool(false));
        parsed.isLive = track.value(QStringLiteral("isLive")).toBool(true);
        // Prefer the MSF-01 shape. The inline initData spelling is MSF-00 and is
        // accepted as a fallback so catalogs from older publishers still resolve.
        const QString initRef = track.value(QStringLiteral("initRef")).toString();
        const QString initDataB64 = track.value(QStringLiteral("initData")).toString();
        if (!initRef.isEmpty() && initDataById.contains(initRef)) {
            parsed.initData = initDataById.value(initRef);
        } else if (!initDataB64.isEmpty()) {
            parsed.initData = QByteArray::fromBase64(initDataB64.toLatin1());
        }

        if (parsed.packetSize != 188 && parsed.packetSize != 192) {
            return fail(QStringLiteral("Unsupported mpeg2tsPacketSize %1 (expected 188 or 192).")
                            .arg(parsed.packetSize));
        }

        if (!msfVersion.isEmpty()) {
            parsed.msfVersion = msfVersion;
        }
        *out = parsed;
        if (mediaTrackName) {
            *mediaTrackName = parsed.track;
        }
        return true;
    }

    if (!unsupportedMode.isEmpty()) {
        return fail(QStringLiteral("Unsupported mpeg2tsMode \"%1\" (expected "
                                   "unmodified-program, unmodified-multiplex, or per-program).")
                        .arg(unsupportedMode));
    }
    return fail(QStringLiteral("Catalog has no mpeg2ts-packaged track."));
}

} // namespace moq2ts
