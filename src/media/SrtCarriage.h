#pragma once

#include <QString>
#include <QStringList>

namespace moq2ts {

// The carriage of SRT ingest. It forwards the source unmodified unless
// --per-program asks for one program; warning names the per-program options
// that unmodified carriage then ignores, or is empty.
struct SrtCarriage {
    bool unmodified = true;
    QString warning;
};

inline SrtCarriage srtCarriage(bool perProgram, bool unmodified, bool retainSi, bool siTables, bool retainNull) {
    SrtCarriage carriage;
    carriage.unmodified = !perProgram;
    if (perProgram || unmodified) {
        return carriage;
    }
    QStringList ignored;
    if (retainSi) {
        ignored << QStringLiteral("--retain-si");
    } else if (siTables) {
        ignored << QStringLiteral("--si");
    }
    if (retainNull) {
        ignored << QStringLiteral("--retain-null");
    }
    if (!ignored.isEmpty()) {
        const bool one = ignored.size() == 1;
        carriage.warning = QStringLiteral("%1 %2 only meaningful in per-program mode; SRT ingest forwards the source "
                                          "unmodified without --per-program, so %3 ignored.")
                               .arg(ignored.join(QStringLiteral(", ")), one ? QStringLiteral("is") : QStringLiteral("are"),
                                    one ? QStringLiteral("it is") : QStringLiteral("they are"));
    }
    return carriage;
}

}  // namespace moq2ts
