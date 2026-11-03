#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
CLI="$REPO_ROOT/src/cli/main_cli.cpp"

fail() { printf '%s\n' "$1" >&2; exit 1; }

# The periodic [stats] line is throttled and comes before the last Group goes
# out, so the CLI prints the totals once more at exit.
grep -q '\[final\] objects=%lld bytes=%lld groups=%lld %s' "$CLI" \
  || fail "the CLI must print a [final] totals line at exit"
grep -q '\[final\] %s' "$CLI" \
  || fail "the --config path must print a [final] totals line at exit"

# What the publisher pulled and what moqxr sent are two counts: the sent
# counts come from moqxr only, never from the pulled ones.
[ "$(grep -c 'finalFields()' "$CLI")" -ge 2 ] \
  || fail "both [final] lines must print pulled_* and sent_* (PublishTotals::finalFields)"
grep -q 'sentTotals' "$CLI" \
  || fail "the sent counts must come from MoqxrPublisher::sentTotals"


# The totals are written from the worker threads: the recorders connect directly.
grep -q 'Qt::DirectConnection' "$CLI" \
  || fail "the totals recorders must use direct connections (the queue is not drained at exit)"

# The line comes after the pipeline has stopped, so it holds the last values.
awk '/pipeline.waitForStopped\(\)/ {w=NR} /\[final\]/ {f=NR} END {exit !(w && f && f > w)}' "$CLI" \
  || fail "the [final] line must be printed after waitForStopped()"

printf 'cli final-stats guards passed\n'
