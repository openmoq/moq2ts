#include "app/PublisherSpec.h"

#include <QStringList>

#include <iostream>
#include <string>

using moq2ts::parsePublisherSpec;
using moq2ts::PublisherSpec;
using moq2ts::SiTable;

namespace {
bool expect(bool cond, const std::string& msg) {
    if (!cond) { std::cerr << "FAIL: " << msg << '\n'; return false; }
    return true;
}

const char* const kExample = R"({
  "relay": { "endpoint": "moqt://relay.example:4433", "namespace": "live/mpts", "draft": 22 },
  "ingest": { "srt": { "mode": "listener", "host": "0.0.0.0", "port": 9000, "latency_ms": 200 } },
  "programs": [
    { "number": 101, "mux_rate_bps": 6000000, "si": ["sdt", "eit-pf", "tdt-tot"] },
    { "number": 102 }
  ],
  "segment_bytes": 65536
})";

// Parses json, which must fail with an error that contains every one of words.
bool rejects(const QByteArray& json, const QStringList& words, const std::string& msg) {
    PublisherSpec spec;
    QString error;
    if (parsePublisherSpec(json, &spec, &error)) {
        return expect(false, msg + ": accepted");
    }
    bool ok = true;
    for (const QString& word : words) {
        ok &= expect(error.contains(word), msg + ": error \"" + error.toStdString() + "\" lacks \"" +
                                               word.toStdString() + "\"");
    }
    return ok;
}

// The example with one replacement made in its text.
QByteArray exampleWith(const char* from, const char* to) {
    QByteArray json(kExample);
    const qsizetype at = json.indexOf(from);
    if (at < 0) {
        std::cerr << "test bug: \"" << from << "\" not in the example\n";
        return {};
    }
    return json.replace(at, qstrlen(from), to);
}
}  // namespace

int main() {
    bool ok = true;

    // The schema example parses into the matching spec.
    {
        PublisherSpec spec;
        QString error;
        const bool parsed = parsePublisherSpec(kExample, &spec, &error);
        ok &= expect(parsed, "example parses: " + error.toStdString());
        ok &= expect(spec.relay.moqEndpoint == QStringLiteral("moqt://relay.example:4433"), "endpoint");
        ok &= expect(spec.relay.namespaceName == QStringLiteral("live/mpts"), "namespace");
        ok &= expect(spec.relay.draftVersion == 22, "draft");
        ok &= expect(!spec.relay.insecure, "the relay certificate is verified by default");
        ok &= expect(spec.srt.mode == QStringLiteral("listener"), "srt mode");
        ok &= expect(spec.srt.host == QStringLiteral("0.0.0.0"), "srt host");
        ok &= expect(spec.srt.port == 9000, "srt port");
        ok &= expect(spec.srt.latencyMs == 200, "srt latency");
        ok &= expect(spec.sourcePath.isEmpty(), "no path with SRT ingest");
        ok &= expect(spec.segmentBytes == 65536, "segment bytes");
        ok &= expect(spec.programs.size() == 2, "two programs");
        if (spec.programs.size() == 2) {
            ok &= expect(spec.programs.at(0).programNumber == 101, "first program number");
            ok &= expect(spec.programs.at(0).muxRateBps == 6000000, "first program mux rate");
            ok &= expect(spec.programs.at(0).siTables ==
                             QSet<SiTable>{SiTable::Sdt, SiTable::EitPresentFollowing, SiTable::TdtTot},
                         "first program SI tables");
            ok &= expect(spec.programs.at(1).programNumber == 102, "second program number");
            ok &= expect(spec.programs.at(1).muxRateBps == 0, "second program works its mux rate out");
            ok &= expect(spec.programs.at(1).siTables.isEmpty(), "second program keeps no SI table");
        }
    }

    // A FIFO ingest sets the path and leaves the SRT spec empty.
    {
        PublisherSpec spec;
        QString error;
        const QByteArray json =
            exampleWith(R"("srt": { "mode": "listener", "host": "0.0.0.0", "port": 9000, "latency_ms": 200 })",
                        R"("path": "/tmp/mpts.ts")");
        const bool parsed = parsePublisherSpec(json, &spec, &error);
        ok &= expect(parsed, "path ingest parses: " + error.toStdString());
        ok &= expect(spec.sourcePath == QStringLiteral("/tmp/mpts.ts"), "path ingest sets the path");
        ok &= expect(spec.srt.mode.isEmpty(), "path ingest leaves the SRT spec empty");
    }

    ok &= rejects(R"({ "relay": { "endpoint": "mock://local", "namespace": "live/mpts" },
                      "ingest": { "path": "/tmp/mpts.ts" } })",
                  {QStringLiteral("programs")}, "missing programs");
    ok &= rejects(exampleWith(R"(["sdt", "eit-pf", "tdt-tot"])", R"(["foo"])"), {QStringLiteral("foo")},
                  "unknown SI table");
    ok &= rejects(exampleWith(R"("mux_rate_bps": 6000000)", R"("mux_rate_bps": -1)"),
                  {QStringLiteral("mux_rate_bps"), QStringLiteral("-1")}, "negative mux rate");
    ok &= rejects(exampleWith(R"("ingest": {)", R"("ingest": { "path": "/tmp/mpts.ts",)"),
                  {QStringLiteral("ingest")}, "both srt and path");
    // No key is reserved: si_overrides is an unknown key, even empty.
    ok &= rejects(exampleWith(R"({ "number": 102 })", R"({ "number": 102, "si_overrides": {} })"),
                  {QStringLiteral("si_overrides")}, "empty si_overrides");

    // A path ingest with every optional field left out takes the defaults.
    {
        PublisherSpec spec;
        QString error;
        const bool parsed = parsePublisherSpec(R"({ "relay": { "endpoint": "mock://local", "namespace": "live/mpts" },
                                                    "ingest": { "path": "/tmp/mpts.ts" },
                                                    "programs": [ { "number": 7 } ] })",
                                               &spec, &error);
        ok &= expect(parsed, "minimal path ingest parses: " + error.toStdString());
        ok &= expect(spec.relay.draftVersion == 22, "draft defaults to 22");
        ok &= expect(spec.segmentBytes == 64 * 1024, "segment bytes default to 65536");
        ok &= expect(spec.srt.mode.isEmpty() && spec.srt.port == 0, "path ingest leaves the SRT spec at its defaults");
        ok &= expect(spec.programs.size() == 1 && spec.programs.at(0).muxRateBps == 0 &&
                         spec.programs.at(0).siTables.isEmpty(),
                     "a program defaults to a worked-out mux rate and no SI table");
    }

    // segment_bytes is at most 1 MiB, the largest Object size the GUI offers.
    ok &= rejects(exampleWith(R"("segment_bytes": 65536)", R"("segment_bytes": 1048577)"),
                  {QStringLiteral("segment_bytes: 1048577 is not an integer from 1 to 1048576.")},
                  "segment bytes over 1 MiB");
    {
        PublisherSpec spec;
        QString error;
        ok &= expect(parsePublisherSpec(exampleWith(R"("segment_bytes": 65536)", R"("segment_bytes": 1048576)"),
                                        &spec, &error) &&
                         spec.segmentBytes == 1048576,
                     "segment bytes of 1 MiB parse: " + error.toStdString());
    }

    // The relay authorization token, opaque to moq2ts: a value or a file, of
    // the token type that the relay expects (16 by default, as moqx).
    {
        PublisherSpec spec;
        QString error;
        ok &= expect(parsePublisherSpec(kExample, &spec, &error) && spec.relay.authToken.isEmpty() &&
                         spec.relay.authTokenFile.isEmpty() && spec.relay.authTokenType == 16,
                     "no token by default, of type 16: " + error.toStdString());
        ok &= expect(parsePublisherSpec(exampleWith(R"("draft": 22)",
                                                    R"("draft": 22, "auth_token": "base64:AAEC", "auth_token_type": 0)"),
                                        &spec, &error) &&
                         spec.relay.authToken == QByteArray("base64:AAEC") && spec.relay.authTokenType == 0,
                     "relay.auth_token and relay.auth_token_type parse: " + error.toStdString());
        ok &= expect(parsePublisherSpec(exampleWith(R"("draft": 22)", R"("draft": 22, "auth_token_file": "/run/token")"),
                                        &spec, &error) &&
                         spec.relay.authTokenFile == QStringLiteral("/run/token"),
                     "relay.auth_token_file parses: " + error.toStdString());
    }
    ok &= rejects(exampleWith(R"("draft": 22)", R"("draft": 22, "auth_token": "x", "auth_token_file": "/run/token")"),
                  {QStringLiteral("relay: holds both \"auth_token\" and \"auth_token_file\"")},
                  "a token given twice");
    {
        // The error names the field, never the token.
        PublisherSpec spec;
        QString error;
        parsePublisherSpec(exampleWith(R"("draft": 22)", R"("draft": 22, "auth_token": 12345678)"), &spec, &error);
        ok &= expect(error.contains(QStringLiteral("relay.auth_token")) && !error.contains(QStringLiteral("12345678")),
                     "a malformed token is named, not echoed: " + error.toStdString());
    }

    // Unknown keys are refused at every level, by their full path.
    ok &= rejects(exampleWith(R"("segment_bytes": 65536)", R"("segment_bytes": 65536, "segment_byte": 1)"),
                  {QStringLiteral("segment_byte: unknown key")}, "unknown root key");
    ok &= rejects(exampleWith(R"("draft": 22)", R"("draft": 22, "drafts": 22)"),
                  {QStringLiteral("relay.drafts: unknown key")}, "unknown relay key");
    ok &= rejects(exampleWith(R"("ingest": {)", R"("ingest": { "fifo": "/tmp/x",)"),
                  {QStringLiteral("ingest.fifo: unknown key")}, "unknown ingest key");
    ok &= rejects(exampleWith(R"("latency_ms": 200)", R"("latency_ms": 200, "latency": 200)"),
                  {QStringLiteral("ingest.srt.latency: unknown key")}, "unknown srt key");
    ok &= rejects(exampleWith(R"("mux_rate_bps": 6000000)", R"("mux_rate": 6000000)"),
                  {QStringLiteral("programs[0].mux_rate: unknown key")}, "unknown program key");

    // The ingest error names the two keys, not the whole object.
    ok &= rejects(exampleWith(R"("srt": { "mode": "listener", "host": "0.0.0.0", "port": 9000, "latency_ms": 200 })",
                              R"("x": 1)"),
                  {QStringLiteral("ingest")}, "neither srt nor path (with an unknown key)");
    {
        PublisherSpec spec;
        QString error;
        parsePublisherSpec(exampleWith(R"("srt": { "mode": "listener", "host": "0.0.0.0", "port": 9000, "latency_ms": 200 })",
                                       R"("srt": {}, "path": "/tmp/x")"),
                           &spec, &error);
        ok &= expect(error == QStringLiteral("ingest: holds both \"srt\" and \"path\"; it holds exactly one of them."),
                     "both error names the keys: " + error.toStdString());
    }
    ok &= rejects(R"({ "relay": { "endpoint": "mock://local", "namespace": "live/mpts" }, "ingest": {},
                      "programs": [ { "number": 1 } ] })",
                  {QStringLiteral("ingest: holds neither \"srt\" nor \"path\"")}, "empty ingest");

    // Values out of range or of the wrong type name the field and the value.
    ok &= rejects(exampleWith(R"("port": 9000)", R"("port": 70000)"),
                  {QStringLiteral("ingest.srt.port"), QStringLiteral("70000")}, "port out of range");
    ok &= rejects(exampleWith(R"("port": 9000)", R"("port": "9000")"),
                  {QStringLiteral("ingest.srt.port"), QStringLiteral("\"9000\"")}, "string port");
    ok &= rejects(exampleWith(R"({ "number": 102 })", R"({ "number": 1.5 })"),
                  {QStringLiteral("programs[1].number"), QStringLiteral("1.5")}, "non-integer program number");
    ok &= rejects(exampleWith(R"({ "number": 102 })", R"({ "number": 101 })"),
                  {QStringLiteral("programs[1].number"), QStringLiteral("101")}, "duplicate program number");
    {
        PublisherSpec spec;
        QString error;
        const bool parsed = parsePublisherSpec(exampleWith(R"("draft": 22)", R"("draft": 22, "insecure": true)"), &spec, &error);
        ok &= expect(parsed, "relay.insecure parses: " + error.toStdString());
        ok &= expect(parsed && spec.relay.insecure, "relay.insecure true is kept");
    }
    ok &= rejects(exampleWith(R"("draft": 22)", R"("draft": 22, "insecure": "yes")"),
                  {QStringLiteral("relay.insecure"), QStringLiteral("\"yes\"")}, "insecure not a boolean");
    ok &= rejects(exampleWith(R"("draft": 22)", R"("draft": 99)"),
                  {QStringLiteral("relay.draft"), QStringLiteral("99")}, "unsupported draft");
    ok &= rejects(exampleWith(R"(["sdt", "eit-pf", "tdt-tot"])", R"("sdt")"),
                  {QStringLiteral("programs[0].si"), QStringLiteral("\"sdt\"")}, "si not an array");
    ok &= rejects(exampleWith(R"(["sdt", "eit-pf", "tdt-tot"])", R"(["sdt", ""])"),
                  {QStringLiteral("programs[0].si: \"\" is not an SI table name")}, "empty SI table name");
    ok &= rejects(exampleWith(R"(["sdt", "eit-pf", "tdt-tot"])", R"(["  "])"),
                  {QStringLiteral("programs[0].si: \"  \" is not an SI table name")}, "blank SI table name");

    if (ok) {
        std::cout << "publisher spec tests passed\n";
    }
    return ok ? 0 : 1;
}
