#include "PublisherSpec.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>
#include <QSet>
#include <QStringList>

#include <cmath>
#include <limits>

#include "../publish/MoqxrPublisher.h"

namespace moq2ts {

namespace {

// The value as JSON text, for an error that names it.
QString jsonText(const QJsonValue& value) {
    if (value.isString()) {
        return QStringLiteral("\"%1\"").arg(value.toString());
    }
    if (value.isUndefined()) {
        return QStringLiteral("nothing");
    }
    return QString::fromUtf8(QJsonDocument(QJsonArray{value}).toJson(QJsonDocument::Compact).mid(1).chopped(1));
}

bool fail(QString* error, const QString& message) {
    if (error != nullptr) {
        *error = message;
    }
    return false;
}

// Refuses a key of object that is not in known, so a misspelt key fails rather
// than being ignored. field names object in errors; empty for the root.
bool checkKeys(const QJsonObject& object, const QString& field, const QStringList& known, QString* error) {
    for (auto it = object.begin(); it != object.end(); ++it) {
        if (!known.contains(it.key())) {
            return fail(error, QStringLiteral("%1%2: unknown key.")
                                   .arg(field.isEmpty() ? QString() : field + QLatin1Char('.'), it.key()));
        }
    }
    return true;
}

// Reads the integer at object[key], named field in errors, into *out. An absent
// key keeps *out, unless required.
bool readInt(const QJsonObject& object, const QString& key, const QString& field, qint64 min, qint64 max,
             bool required, qint64* out, QString* error) {
    const QJsonValue value = object.value(key);
    if (value.isUndefined() && !required) {
        return true;
    }
    const double number = value.toDouble(std::numeric_limits<double>::quiet_NaN());
    if (!value.isDouble() || std::floor(number) != number || number < double(min) || number > double(max)) {
        return fail(error, QStringLiteral("%1: %2 is not an integer from %3 to %4.")
                               .arg(field, jsonText(value))
                               .arg(min)
                               .arg(max));
    }
    *out = static_cast<qint64>(number);
    return true;
}

// Reads the non-empty string at object[key], named field in errors.
bool readString(const QJsonObject& object, const QString& key, const QString& field, bool required, QString* out,
                QString* error) {
    const QJsonValue value = object.value(key);
    if (value.isUndefined() && !required) {
        return true;
    }
    if (!value.isString() || value.toString().isEmpty()) {
        return fail(error, QStringLiteral("%1: %2 is not a non-empty string.").arg(field, jsonText(value)));
    }
    *out = value.toString();
    return true;
}

bool readObject(const QJsonObject& object, const QString& key, const QString& field, QJsonObject* out,
                QString* error) {
    const QJsonValue value = object.value(key);
    if (!value.isObject()) {
        return fail(error, QStringLiteral("%1: %2 is not an object.").arg(field, jsonText(value)));
    }
    *out = value.toObject();
    return true;
}

bool readRelay(const QJsonObject& root, PublishConfig* relay, QString* error) {
    QJsonObject object;
    if (!readObject(root, QStringLiteral("relay"), QStringLiteral("relay"), &object, error) ||
        !checkKeys(object, QStringLiteral("relay"),
                   {QStringLiteral("endpoint"), QStringLiteral("namespace"), QStringLiteral("draft"),
                    QStringLiteral("insecure"), QStringLiteral("auth_token"), QStringLiteral("auth_token_file"),
                    QStringLiteral("auth_token_type")},
                   error) ||
        !readString(object, QStringLiteral("endpoint"), QStringLiteral("relay.endpoint"), true, &relay->moqEndpoint,
                    error) ||
        !readString(object, QStringLiteral("namespace"), QStringLiteral("relay.namespace"), true,
                    &relay->namespaceName, error)) {
        return false;
    }
    qint64 draft = relay->draftVersion;
    if (!readInt(object, QStringLiteral("draft"), QStringLiteral("relay.draft"), 0,
                 std::numeric_limits<int>::max(), false, &draft, error)) {
        return false;
    }
    if (!MoqxrPublisher::supportsDraft(static_cast<int>(draft))) {
        return fail(error, QStringLiteral("relay.draft: %1 is not supported; use %2.")
                               .arg(draft)
                               .arg(MoqxrPublisher::supportedDrafts()));
    }
    relay->draftVersion = static_cast<int>(draft);
    if (object.contains(QStringLiteral("auth_token")) && object.contains(QStringLiteral("auth_token_file"))) {
        return fail(error, QStringLiteral("relay: holds both \"auth_token\" and \"auth_token_file\"; it holds one "
                                          "of them at most."));
    }
    // The token is a secret: an error names the field, never the value.
    const QJsonValue token = object.value(QStringLiteral("auth_token"));
    if (!token.isUndefined()) {
        if (!token.isString() || token.toString().isEmpty()) {
            return fail(error, QStringLiteral("relay.auth_token: not a non-empty string."));
        }
        relay->authToken = token.toString().toUtf8();
    }
    qint64 tokenType = static_cast<qint64>(relay->authTokenType);
    if (!readString(object, QStringLiteral("auth_token_file"), QStringLiteral("relay.auth_token_file"), false,
                    &relay->authTokenFile, error) ||
        !readInt(object, QStringLiteral("auth_token_type"), QStringLiteral("relay.auth_token_type"), 0,
                 kMaxAuthTokenType, false, &tokenType, error)) {
        return false;
    }
    relay->authTokenType = static_cast<quint64>(tokenType);
    const QJsonValue insecure = object.value(QStringLiteral("insecure"));
    if (!insecure.isUndefined()) {
        if (!insecure.isBool()) {
            return fail(error, QStringLiteral("relay.insecure: %1 is not true or false.").arg(jsonText(insecure)));
        }
        relay->insecure = insecure.toBool();
    }
    return true;
}

bool readIngest(const QJsonObject& root, PublisherSpec* spec, QString* error) {
    QJsonObject object;
    if (!readObject(root, QStringLiteral("ingest"), QStringLiteral("ingest"), &object, error) ||
        !checkKeys(object, QStringLiteral("ingest"), {QStringLiteral("srt"), QStringLiteral("path")}, error)) {
        return false;
    }
    const bool hasSrt = object.contains(QStringLiteral("srt"));
    const bool hasPath = object.contains(QStringLiteral("path"));
    if (hasSrt == hasPath) {
        return fail(error, hasSrt ? QStringLiteral("ingest: holds both \"srt\" and \"path\"; it holds exactly one of them.")
                                  : QStringLiteral("ingest: holds neither \"srt\" nor \"path\"; it holds exactly one of them."));
    }
    if (hasPath) {
        return readString(object, QStringLiteral("path"), QStringLiteral("ingest.path"), true, &spec->sourcePath,
                          error);
    }
    QJsonObject srt;
    if (!readObject(object, QStringLiteral("srt"), QStringLiteral("ingest.srt"), &srt, error) ||
        !checkKeys(srt, QStringLiteral("ingest.srt"),
                   {QStringLiteral("mode"), QStringLiteral("host"), QStringLiteral("port"), QStringLiteral("latency_ms")},
                   error)) {
        return false;
    }
    SrtIngestSpec ingest;
    ingest.mode = QStringLiteral("listener");
    if (!readString(srt, QStringLiteral("mode"), QStringLiteral("ingest.srt.mode"), false, &ingest.mode, error)) {
        return false;
    }
    if (ingest.mode != QStringLiteral("listener") && ingest.mode != QStringLiteral("caller")) {
        return fail(error, QStringLiteral("ingest.srt.mode: \"%1\" is not \"listener\" or \"caller\".")
                               .arg(ingest.mode));
    }
    // As with --srt-config: a listener binds every interface by default.
    const bool listener = ingest.mode == QStringLiteral("listener");
    ingest.host = listener ? QStringLiteral("0.0.0.0") : QStringLiteral("127.0.0.1");
    qint64 port = 9000;
    qint64 latencyMs = 120;
    if (!readString(srt, QStringLiteral("host"), QStringLiteral("ingest.srt.host"), false, &ingest.host, error) ||
        !readInt(srt, QStringLiteral("port"), QStringLiteral("ingest.srt.port"), 1, 65535, false, &port, error) ||
        !readInt(srt, QStringLiteral("latency_ms"), QStringLiteral("ingest.srt.latency_ms"), 0,
                 std::numeric_limits<int>::max(), false, &latencyMs, error)) {
        return false;
    }
    ingest.port = static_cast<int>(port);
    ingest.latencyMs = static_cast<int>(latencyMs);
    spec->srt = ingest;
    return true;
}

bool readProgram(const QJsonValue& value, const QString& field, MptsProgramConfig* program, QString* error) {
    if (!value.isObject()) {
        return fail(error, QStringLiteral("%1: %2 is not an object.").arg(field, jsonText(value)));
    }
    const QJsonObject object = value.toObject();
    if (!checkKeys(object, field,
                   {QStringLiteral("number"), QStringLiteral("mux_rate_bps"), QStringLiteral("si")},
                   error)) {
        return false;
    }
    // program_number 0 points at the NIT in the PAT; programs are 1 to 65535.
    qint64 number = 0;
    qint64 muxRate = 0;
    if (!readInt(object, QStringLiteral("number"), field + QStringLiteral(".number"), 1, 65535, true, &number,
                 error) ||
        !readInt(object, QStringLiteral("mux_rate_bps"), field + QStringLiteral(".mux_rate_bps"), 0,
                 std::numeric_limits<qint64>::max() / 2, false, &muxRate, error)) {
        return false;
    }
    program->programNumber = static_cast<int>(number);
    program->muxRateBps = muxRate;
    const QJsonValue si = object.value(QStringLiteral("si"));
    if (!si.isUndefined()) {
        if (!si.isArray()) {
            return fail(error, QStringLiteral("%1.si: %2 is not an array of SI table names.")
                                   .arg(field, jsonText(si)));
        }
        for (const QJsonValue& name : si.toArray()) {
            QSet<SiTable> tables;
            QString siError;
            if (!name.isString() || name.toString().trimmed().isEmpty() || name.toString().contains(QLatin1Char(','))) {
                return fail(error, QStringLiteral("%1.si: %2 is not an SI table name.").arg(field, jsonText(name)));
            }
            if (!parseSiTables(name.toString(), &tables, &siError)) {
                return fail(error, QStringLiteral("%1.si: %2").arg(field, siError));
            }
            program->siTables.unite(tables);
        }
    }
    return true;
}

bool readPrograms(const QJsonObject& root, QList<MptsProgramConfig>* programs, QString* error) {
    const QJsonValue value = root.value(QStringLiteral("programs"));
    if (!value.isArray() || value.toArray().isEmpty()) {
        return fail(error, QStringLiteral("programs: %1 is not a non-empty array of programs.").arg(jsonText(value)));
    }
    const QJsonArray array = value.toArray();
    QSet<int> numbers;
    for (qsizetype index = 0; index < array.size(); ++index) {
        const QString field = QStringLiteral("programs[%1]").arg(index);
        MptsProgramConfig program;
        if (!readProgram(array.at(index), field, &program, error)) {
            return false;
        }
        // One track per program: program-<program_number> must be unique.
        if (numbers.contains(program.programNumber)) {
            return fail(error, QStringLiteral("%1.number: %2 is listed twice.").arg(field).arg(program.programNumber));
        }
        numbers.insert(program.programNumber);
        programs->append(program);
    }
    return true;
}

} // namespace

// The largest target Object size, as the GUI's: four Objects per program
// wait for the publisher (MptsPipeline), so this bounds their memory too.
constexpr qint64 kMaxSegmentBytes = 1024 * 1024;

bool parsePublisherSpec(const QByteArray& json, PublisherSpec* out, QString* error) {
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(json, &parseError);
    if (parseError.error != QJsonParseError::NoError) {
        return fail(error, QStringLiteral("Not valid JSON at offset %1: %2.")
                               .arg(parseError.offset)
                               .arg(parseError.errorString()));
    }
    if (!document.isObject()) {
        return fail(error, QStringLiteral("The configuration is not a JSON object."));
    }
    const QJsonObject root = document.object();
    if (!checkKeys(root, QString(),
                   {QStringLiteral("relay"), QStringLiteral("ingest"), QStringLiteral("programs"),
                    QStringLiteral("segment_bytes"), QStringLiteral("timeline"), QStringLiteral("accessible_window_ms")},
                   error)) {
        return false;
    }
    PublisherSpec spec;
    qint64 segmentBytes = spec.segmentBytes;
    // As --accessible-window, which takes up to 2,000,000 s.
    qint64 windowMs = spec.relay.accessibleWindowMs;
    if (!readRelay(root, &spec.relay, error) || !readIngest(root, &spec, error) ||
        !readPrograms(root, &spec.programs, error) ||
        !readInt(root, QStringLiteral("segment_bytes"), QStringLiteral("segment_bytes"), 1, kMaxSegmentBytes, false,
                 &segmentBytes, error) ||
        !readInt(root, QStringLiteral("accessible_window_ms"), QStringLiteral("accessible_window_ms"), 0,
                 2000000000, false, &windowMs, error)) {
        return false;
    }
    spec.segmentBytes = static_cast<int>(segmentBytes);
    spec.relay.accessibleWindowMs = static_cast<int>(windowMs);
    const QJsonValue timeline = root.value(QStringLiteral("timeline"));
    if (!timeline.isUndefined()) {
        if (!timeline.isBool()) {
            return fail(error, QStringLiteral("timeline: %1 is not true or false.").arg(jsonText(timeline)));
        }
        spec.relay.timelineTrack = timeline.toBool();
    }
    if (out != nullptr) {
        *out = spec;
    }
    return true;
}

} // namespace moq2ts
