#!/usr/bin/env bash
set -euo pipefail

# Guards for SRT ingest of an unmodified multiplex (draft "Unmodified Multiplex").
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
CLI="$REPO_ROOT/src/cli/main_cli.cpp"
PIPE="$REPO_ROOT/src/media/LivePipeline.cpp"
SRT="$REPO_ROOT/src/media/SrtSource.cpp"

fail() { printf '%s\n' "$1" >&2; exit 1; }

# Existing command lines keep working: --unmodified and --transparent stay, and
# --publish-multiplex names the same carriage.
for name in '"unmodified"' '"transparent"' '"publish-multiplex"'; do
  grep -q "QStringLiteral($name)" "$CLI" || fail "CLI must accept --${name//\"/}"
done
# SRT ingest without a flag still forwards the source unmodified; --per-program
# opts into per-program carriage, and cannot be combined with --unmodified.
# The decision is moq2ts::srtCarriage (tests/srt_carriage_test.cpp).
CARRIAGE="$REPO_ROOT/src/media/SrtCarriage.h"
grep -q 'carriage.unmodified = !perProgram;' "$CARRIAGE" \
  || fail "SRT ingest must keep unmodified carriage by default"
grep -q 'moq2ts::srtCarriage(perProgram' "$CLI" || fail "SRT ingest must take its carriage from srtCarriage"
grep -q 'cfg.transparentMode = cfg.transparentMode || carriage.unmodified;' "$CLI" \
  || fail "SRT ingest must apply the carriage srtCarriage returns"
grep -q 'QStringLiteral("per-program")' "$CLI" || fail "CLI must accept --per-program"
grep -q 'perProgram && cfg.transparentMode' "$CLI" || fail "--per-program and --unmodified must be refused together"

# The multiplex track is mux-N, or mux-N-pP when --program names a reference.
# N counts the sessions (a PAT change starts a new one), so the first is mux-1.
grep -q 'QStringLiteral("mux-%1").arg(generation)' "$PIPE" || fail "multiplex track must be named mux-<session>"
grep -q 'QStringLiteral("mux-%1-p%2").arg(generation)' "$PIPE" || fail "a named reference program must be in the track name"
grep -q 'for (int generation = 1;; ++generation)' "$PIPE" || fail "the first session must be number 1, so that the track is mux-1"
grep -q 'packetizer.namesProgram() ? packetizer.pcrPid() : -1' "$PIPE" \
  || fail "the catalog must name no reference program without --program"

# After a drop, the SRT ring buffer resyncs on the PID that starts Groups.
grep -q 'cfg.resyncPid = resyncPid;' "$CLI" || fail "CLI must share the resync PID with the pipeline"
grep -q 'resyncPid->store(' "$PIPE" || fail "pipeline must publish the Group PID for the SRT resync"
grep -q 'ring.resyncToRandomAccess(resyncPid' "$SRT" || fail "SRT receive loop must resync on the Group PID"

printf 'srt-carriage-choice guards passed\n'
