#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
main_cpp="$repo_root/src/main.cpp"
window_cpp="$repo_root/src/app/MainWindow.cpp"
window_h="$repo_root/src/app/MainWindow.h"
publisher_cpp="$repo_root/src/publish/MoqxrPublisher.cpp"

python3 - "$main_cpp" "$window_cpp" "$window_h" "$publisher_cpp" <<'PY'
from pathlib import Path
import re
import sys

main_cpp = Path(sys.argv[1]).read_text()
window_cpp = Path(sys.argv[2]).read_text()
window_h = Path(sys.argv[3]).read_text()
publisher_cpp = Path(sys.argv[4]).read_text()

required_window = [
    'tabs->addTab(statsTab, QStringLiteral("Stats"))',
    'publisherObjectsValue',
    'publisherBytesValue',
    'publisherTrackValue',
    'pipelineObjectsValue',
    'pipelineBytesValue',
    'resetStats()',
    'void MainWindow::onPublisherFramePublished',
    'formatByteCount',
]
for needle in required_window:
    if needle not in window_cpp and needle not in window_h:
        raise SystemExit(f"missing stats tab UI pattern: {needle}")

if "onPublisherFramePublished(const QString& track, int64_t bytes, int64_t objects)" not in window_h:
    raise SystemExit("main window must expose a slot for publisher frame stats")

if "QObject::connect(&publisher, &moq2ts::MoqxrPublisher::framePublished" not in main_cpp:
    raise SystemExit("publisher framePublished signal must be connected to the stats tab")

# During the run the tab shows what the publisher pulled (framePublished);
# at the end of a successful publish, what moqxr sent (sentTotals), as before
# the two counts were split.
if "onPublisherSentTotals(int64_t objects, int64_t bytes)" not in window_h:
    raise SystemExit("main window must expose a slot for moqxr's sent totals")
if "QObject::connect(&publisher, &moq2ts::MoqxrPublisher::sentTotals" not in main_cpp:
    raise SystemExit("publisher sentTotals signal must be connected to the stats tab")

if "emit framePublished(QStringLiteral(\"catalog\")" not in publisher_cpp:
    raise SystemExit("real publisher must emit catalog stats")

# Match the emit by shape, not by the local variable's name. The publish loop
# holds the object in "current" rather than "next" since the one-object look-ahead
# was introduced; the guarded behaviour is that a per-object stats signal carries
# the track name of the object being published.
if not re.search(r"emit framePublished\(\w+(?:->|\.)trackName", publisher_cpp):
    raise SystemExit("real publisher must emit per-object publisher stats")
PY
