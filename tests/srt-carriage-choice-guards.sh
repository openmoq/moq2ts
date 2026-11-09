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
# SRT ingest without a flag still forwards the source unmodified.
grep -q 'cfg.transparentMode = true;' "$CLI" \
  || fail "SRT ingest must keep unmodified carriage by default"

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
