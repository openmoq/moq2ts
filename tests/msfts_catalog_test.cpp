#include "media/MsftsMuxer.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <iostream>
#include <string>

using moq2ts::MsftsCatalog;
using moq2ts::MsftsMuxer;
using moq2ts::Mpeg2tsMode;

namespace {
bool expect(bool cond, const std::string& msg) {
    if (!cond) { std::cerr << "FAIL: " << msg << '\n'; return false; }
    return true;
}

MsftsCatalog baseCatalog(Mpeg2tsMode mode) {
    MsftsCatalog catalog;
    catalog.track = QStringLiteral("program-1");
    catalog.mode = mode;
    catalog.programNumber = 1;
    catalog.pcrPid = 256;
    catalog.mpeg2tsMuxRateBps = 8000000;
    catalog.siPids = {0x11};
    catalog.initData = QByteArray(188, char(0x47));
    catalog.timelineTrack = QStringLiteral("program-1.timeline");
    return catalog;
}

QJsonObject mediaTrack(const QByteArray& json) {
    return QJsonDocument::fromJson(json).object().value(QStringLiteral("tracks")).toArray().at(0).toObject();
}
}  // namespace

int main() {
    bool ok = true;

    // unmodified-program describes its one program; SI PIDs are per-program only.
    {
        const QJsonObject track = mediaTrack(MsftsMuxer::catalogJson(baseCatalog(Mpeg2tsMode::UnmodifiedProgram)));
        ok &= expect(track.value("mpeg2tsMode").toString() == "unmodified-program", "unmodified-program: mode");
        ok &= expect(track.value("mpeg2tsProgramNumber").toInt() == 1, "unmodified-program: program number");
        ok &= expect(track.value("mpeg2tsPcrPid").toInt() == 256, "unmodified-program: PCR PID");
        ok &= expect(!track.contains("mpeg2tsMuxRate"), "unmodified-program: no mux rate");
        ok &= expect(!track.contains("mpeg2tsSiPids"), "unmodified-program: no SI PIDs");
    }

    // accessibleWindowTime (editor's copy of MSF) is declared on a live media
    // track with a window, and on no other.
    {
        MsftsCatalog catalog = baseCatalog(Mpeg2tsMode::PerProgram);
        catalog.isLive = true;
        ok &= expect(!mediaTrack(MsftsMuxer::catalogJson(catalog)).contains("accessibleWindowTime"),
                     "window: absent without a window");
        catalog.accessibleWindowMs = 300000;
        ok &= expect(mediaTrack(MsftsMuxer::catalogJson(catalog)).value("accessibleWindowTime").toInt() == 300000,
                     "window: declared in ms on a live track");
        catalog.isLive = false;
        ok &= expect(!mediaTrack(MsftsMuxer::catalogJson(catalog)).contains("accessibleWindowTime"),
                     "window: absent on a VOD track");
    }

    // The role is the content of the track (MSF 5.2.6): written as set, and
    // omitted when empty, as for a multiplex.
    {
        MsftsCatalog catalog = baseCatalog(Mpeg2tsMode::PerProgram);
        ok &= expect(mediaTrack(MsftsMuxer::catalogJson(catalog)).value("role").toString() == "video", "role: video");
        catalog.role = QStringLiteral("audio");
        ok &= expect(mediaTrack(MsftsMuxer::catalogJson(catalog)).value("role").toString() == "audio", "role: audio");
        catalog.role.clear();
        ok &= expect(!mediaTrack(MsftsMuxer::catalogJson(catalog)).contains("role"), "role: omitted when empty");
    }

    // unmodified-multiplex with a reference program: its number and PCR PID,
    // and random access when declared. The mux rate and SI PIDs stay out.
    {
        MsftsCatalog catalog = baseCatalog(Mpeg2tsMode::UnmodifiedMultiplex);
        catalog.randomAccess = true;
        const QJsonObject track = mediaTrack(MsftsMuxer::catalogJson(catalog));
        ok &= expect(track.value("mpeg2tsMode").toString() == "unmodified-multiplex", "multiplex: mode");
        ok &= expect(track.value("mpeg2tsProgramNumber").toInt() == 1 && track.value("mpeg2tsPcrPid").toInt() == 256,
                     "multiplex: reference program named");
        ok &= expect(track.value("mpeg2tsRandomAccess").toBool(), "multiplex: random access of the reference program");
        ok &= expect(track.value("initRef").toString() == "init-program-1",
                     "multiplex with reference: initRef (draft: SHOULD when random access is declared)");
        for (const char* field : {"mpeg2tsMuxRate", "mpeg2tsSiPids", "mpeg2tsScte35Pid", "altGroup", "mpeg2tsEsPid"}) {
            ok &= expect(!track.contains(QLatin1String(field)), std::string("multiplex: no ") + field);
        }
        MsftsCatalog parsed;
        ok &= expect(MsftsMuxer::catalogFromJson(MsftsMuxer::catalogJson(catalog), &parsed, nullptr, nullptr) &&
                         parsed.mode == Mpeg2tsMode::UnmodifiedMultiplex && parsed.programNumber == 1 &&
                         parsed.pcrPid == 256,
                     "multiplex: the parser reads the reference program");
    }

    // unmodified-multiplex without a reference program: no program fields, and
    // no mpeg2tsRandomAccess even if the flag is set.
    {
        MsftsCatalog catalog = baseCatalog(Mpeg2tsMode::UnmodifiedMultiplex);
        catalog.pcrPid = -1;
        catalog.randomAccess = true;
        const QJsonObject track = mediaTrack(MsftsMuxer::catalogJson(catalog));
        for (const char* field : {"mpeg2tsProgramNumber", "mpeg2tsPcrPid", "mpeg2tsMuxRate", "mpeg2tsSiPids",
                                  "mpeg2tsRandomAccess", "mpeg2tsScte35Pid", "altGroup", "mpeg2tsEsPid"}) {
            ok &= expect(!track.contains(QLatin1String(field)), std::string("multiplex without reference: no ") + field);
        }
        ok &= expect(track.contains("initRef"), "multiplex without reference: initRef for the PAT and every PMT");
    }

    // per-program keeps every program field.
    {
        const QJsonObject track = mediaTrack(MsftsMuxer::catalogJson(baseCatalog(Mpeg2tsMode::PerProgram)));
        ok &= expect(track.value("mpeg2tsMode").toString() == "per-program", "per-program: mode");
        ok &= expect(track.value("mpeg2tsProgramNumber").toInt() == 1, "per-program: program number");
        ok &= expect(track.value("mpeg2tsSiPids").toArray().size() == 1, "per-program: SI PIDs");
        ok &= expect(static_cast<qint64>(track.value("mpeg2tsMuxRate").toDouble()) == 8000000,
                     "per-program: mux rate");
    }

    // initDataList follows the tracks array (MSF 5.1.7), and the document parses.
    {
        MsftsCatalog catalog = baseCatalog(Mpeg2tsMode::PerProgram);
        catalog.generatedAtMs = 1;
        const QByteArray json = MsftsMuxer::catalogJson(catalog);
        const qsizetype tracksAt = json.indexOf("\"tracks\"");
        const qsizetype listAt = json.indexOf("\"initDataList\"");
        ok &= expect(tracksAt >= 0 && listAt > tracksAt, "initDataList after tracks");
        MsftsCatalog parsed;
        ok &= expect(MsftsMuxer::catalogFromJson(json, &parsed, nullptr, nullptr) &&
                         parsed.initData == catalog.initData,
                     "initData resolves through initRef");
    }

    // Several per-program tracks share one catalog (MSF 5.1.7: one initDataList
    // after tracks, one entry per track that has initData).
    {
        MsftsCatalog a = baseCatalog(Mpeg2tsMode::PerProgram);
        a.track = QStringLiteral("program-101");
        a.timelineTrack = QStringLiteral("program-101.timeline");
        a.programNumber = 101;
        a.mpeg2tsMuxRateBps = 4000000;
        a.siPids = {0x11};
        a.generatedAtMs = 7;
        MsftsCatalog b = baseCatalog(Mpeg2tsMode::PerProgram);
        b.track = QStringLiteral("program-102");
        b.timelineTrack = QStringLiteral("program-102.timeline");
        b.programNumber = 102;
        b.mpeg2tsMuxRateBps = 6000000;
        b.siPids = {0x11, 0x12};
        b.initData = QByteArray(188, char(0x48));
        const QByteArray json = MsftsMuxer::catalogJson(QList<MsftsCatalog>{a, b});
        const QJsonObject root = QJsonDocument::fromJson(json).object();
        const QJsonArray tracks = root.value("tracks").toArray();
        ok &= expect(tracks.size() == 4, "two programs: four tracks");
        ok &= expect(tracks.at(0).toObject().value("name").toString() == "program-101" &&
                         tracks.at(1).toObject().value("name").toString() == "program-101.timeline" &&
                         tracks.at(2).toObject().value("name").toString() == "program-102" &&
                         tracks.at(3).toObject().value("name").toString() == "program-102.timeline",
                     "two programs: track order");
        ok &= expect(tracks.at(1).toObject().value("depends").toArray().at(0).toString() == "program-101" &&
                         tracks.at(3).toObject().value("depends").toArray().at(0).toString() == "program-102",
                     "two programs: each timeline depends on its own track");
        const QJsonObject ta = tracks.at(0).toObject();
        const QJsonObject tb = tracks.at(2).toObject();
        ok &= expect(ta.value("mpeg2tsProgramNumber").toInt() == 101 && tb.value("mpeg2tsProgramNumber").toInt() == 102,
                     "two programs: program numbers");
        ok &= expect(ta.value("mpeg2tsMuxRate").toInteger() == 4000000 && tb.value("mpeg2tsMuxRate").toInteger() == 6000000,
                     "two programs: mux rates");
        ok &= expect(ta.value("mpeg2tsSiPids").toArray().size() == 1 && tb.value("mpeg2tsSiPids").toArray().size() == 2,
                     "two programs: SI PIDs");
        ok &= expect(ta.value("initRef").toString() == "init-program-101" &&
                         tb.value("initRef").toString() == "init-program-102",
                     "two programs: initRef");
        const QJsonArray list = root.value("initDataList").toArray();
        ok &= expect(list.size() == 2 && list.at(0).toObject().value("id").toString() == "init-program-101" &&
                         list.at(1).toObject().value("id").toString() == "init-program-102",
                     "two programs: initDataList order");
        ok &= expect(json.indexOf("\"initDataList\"") > json.indexOf("\"tracks\""), "two programs: list after tracks");
        ok &= expect(root.value("generatedAt").toInteger() == 7, "two programs: root fields from the first entry");

        // A track without initData adds no entry.
        a.initData.clear();
        const QJsonObject partial = QJsonDocument::fromJson(MsftsMuxer::catalogJson(QList<MsftsCatalog>{a, b})).object();
        ok &= expect(partial.value("initDataList").toArray().size() == 1 &&
                         !partial.value("tracks").toArray().at(0).toObject().contains("initRef"),
                     "two programs: no entry without initData");

        // The empty list yields an empty tracks array and no initDataList.
        const QJsonObject empty = QJsonDocument::fromJson(MsftsMuxer::catalogJson(QList<MsftsCatalog>{})).object();
        ok &= expect(empty.value("tracks").toArray().isEmpty() && !empty.contains("initDataList"),
                     "empty list: empty tracks, no initDataList");
    }

    // Media timeline records take the floor in milliseconds (MSF 7.1.1).
    ok &= expect(MsftsMuxer::mediaTimelineRecord(10010999, 3, 0, 1759924158381999) == "[10010,[3,0],1759924158381]",
                 "timeline record floors both times");
    ok &= expect(MsftsMuxer::mediaTimelineRecord(999, 0, 0, 0) == "[0,[0,0],0]",
                 "timeline record floors below one millisecond");

    // The media timeline track is identified by packaging (MSF Table 3).
    {
        const QJsonArray tracks = QJsonDocument::fromJson(MsftsMuxer::catalogJson(baseCatalog(Mpeg2tsMode::PerProgram)))
                                      .object().value(QStringLiteral("tracks")).toArray();
        const QJsonObject timeline = tracks.at(1).toObject();
        ok &= expect(timeline.value("packaging").toString() == "mediatimeline", "timeline: packaging");
        ok &= expect(timeline.value("isLive").toBool(), "timeline: required isLive for live catalog");
        ok &= expect(!timeline.contains("namespace"), "timeline: inherit catalog namespace");
        ok &= expect(!timeline.contains("type"), "timeline: no type field");
        ok &= expect(timeline.value("depends").toArray().at(0).toString() == "program-1", "timeline: depends");
    }

    // The parser reads each TS-packet mode back, and skips es-packets.
    for (const Mpeg2tsMode mode : {Mpeg2tsMode::UnmodifiedProgram, Mpeg2tsMode::UnmodifiedMultiplex,
                                   Mpeg2tsMode::PerProgram}) {
        MsftsCatalog parsed;
        QString error;
        ok &= expect(MsftsMuxer::catalogFromJson(MsftsMuxer::catalogJson(baseCatalog(mode)), &parsed, nullptr, &error),
                     "round trip parses: " + error.toStdString());
        ok &= expect(parsed.mode == mode, "round trip keeps the mode");
    }
    {
        MsftsCatalog parsed;
        QString error;
        const QByteArray esPackets = R"({"version":"draft-01","tracks":[{"name":"video","packaging":"mpeg2ts",)"
                                     R"("mpeg2tsMode":"es-packets","mpeg2tsPacketSize":188,"mpeg2tsEsPid":256}]})";
        const bool accepted = MsftsMuxer::catalogFromJson(esPackets, &parsed, nullptr, &error);
        ok &= expect(!accepted && error.contains("es-packets"), "es-packets track is skipped");
    }

    // The catalog that ends a live broadcast (MSF Section 11.3): isComplete
    // true and an empty tracks array, after the version and generatedAt
    // (example of Section 5.6.13).
    {
        MsftsCatalog live = baseCatalog(Mpeg2tsMode::UnmodifiedProgram);
        live.generatedAtMs = 1746104606044;
        const QByteArray json = MsftsMuxer::completeCatalogJson(live);
        const QJsonObject root = QJsonDocument::fromJson(json).object();
        ok &= expect(root.value("version").toString() == "draft-01", "complete catalog: version");
        ok &= expect(static_cast<qint64>(root.value("generatedAt").toDouble()) == 1746104606044,
                     "complete catalog: generatedAt");
        ok &= expect(root.value("isComplete").toBool(), "complete catalog: isComplete true");
        ok &= expect(root.value("tracks").isArray() && root.value("tracks").toArray().isEmpty(),
                     "complete catalog: empty tracks");
        ok &= expect(!root.contains("initDataList") && root.size() == 4, "complete catalog: no other field");
    }

    // Legacy boolean keys still map to a mode.
    {
        MsftsCatalog parsed;
        const QByteArray legacy = R"({"version":"draft-01","tracks":[{"name":"t","packaging":"mpeg2ts","m2tsMpts":true}]})";
        ok &= expect(MsftsMuxer::catalogFromJson(legacy, &parsed, nullptr, nullptr) &&
                         parsed.mode == Mpeg2tsMode::UnmodifiedMultiplex,
                     "legacy m2tsMpts maps to unmodified-multiplex");
    }

    if (ok) {
        std::cout << "msfts catalog tests passed\n";
    }
    // The mux rate worked out from a program's measured rate: 10 % headroom,
    // rounded up to a multiple of 10,000 bit/s; none without a measure.
    ok &= expect(moq2ts::workedOutMuxRate(4000000) == 4400000, "worked-out mux rate: 4,000,000 gives 4,400,000");
    ok &= expect(moq2ts::workedOutMuxRate(1234567) == 1360000, "worked-out mux rate: rounds up to 10,000");
    ok &= expect(moq2ts::workedOutMuxRate(0) == 0 && moq2ts::workedOutMuxRate(-5) == 0,
                 "worked-out mux rate: 0 without a measure");

    return ok ? 0 : 1;
}
