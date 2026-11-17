#include "media/SrtCarriage.h"

#include <iostream>
#include <string>

using moq2ts::srtCarriage;

namespace {
bool expect(bool cond, const std::string& msg) {
    if (!cond) { std::cerr << "FAIL: " << msg << '\n'; return false; }
    return true;
}
}  // namespace

int main() {
    bool ok = true;

    // Without a flag, SRT ingest stays unmodified, and warns about the
    // per-program options it ignores.
    {
        const auto c = srtCarriage(false, false, false, true, true);
        ok &= expect(c.unmodified, "default: unmodified");
        ok &= expect(c.warning.startsWith(QStringLiteral("--si, --retain-null are only meaningful")) &&
                         c.warning.contains(QStringLiteral("without --per-program, so they are ignored")),
                     "default: warns about --si and --retain-null: " + c.warning.toStdString());
    }
    {
        const auto c = srtCarriage(false, false, true, true, false);
        ok &= expect(c.warning.startsWith(QStringLiteral("--retain-si is only meaningful")) &&
                         c.warning.endsWith(QStringLiteral("so it is ignored.")),
                     "default: --retain-si takes the place of --si: " + c.warning.toStdString());
    }
    {
        const auto c = srtCarriage(false, false, false, false, false);
        ok &= expect(c.unmodified && c.warning.isEmpty(), "default without options: no warning");
    }

    // --per-program: per-program carriage, and the options apply.
    {
        const auto c = srtCarriage(true, false, false, true, true);
        ok &= expect(!c.unmodified && c.warning.isEmpty(), "--per-program: per-program, no warning");
    }

    // --unmodified: unmodified, and no warning (the user asked for it).
    {
        const auto c = srtCarriage(false, true, false, true, false);
        ok &= expect(c.unmodified && c.warning.isEmpty(), "--unmodified: unmodified, no warning");
    }

    std::cerr << (ok ? "OK\n" : "FAILED\n");
    return ok ? 0 : 1;
}
