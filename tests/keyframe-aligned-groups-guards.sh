#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
CAPTURE="$REPO_ROOT/src/media/LibavCaptureSource.cpp"
HDR="$REPO_ROOT/src/media/LibavCaptureSource.h"
MUXER="$REPO_ROOT/src/media/MsftsMuxer.cpp"
M2TS_HDR="$REPO_ROOT/src/media/M2tsPacketizer.h"

fail() { printf '%s\n' "$1" >&2; exit 1; }

grep -q 'bool startsGroup' "$M2TS_HDR" \
  || fail "M2tsObject must carry a startsGroup flag"

grep -q 'AV_PKT_FLAG_KEY' "$CAPTURE" \
  || fail "capture must detect keyframes via AV_PKT_FLAG_KEY"

grep -q 'groupBoundaries.push_back' "$CAPTURE" \
  || fail "capture must record a group boundary at each keyframe"

grep -q 'groupBoundaries' "$CAPTURE" \
  || fail "capture must track keyframe group boundaries"

grep -q 'object->startsGroup = true;' "$CAPTURE" \
  || fail "readObject must mark the first object of each group"

grep -q 'randomAccessActive' "$CAPTURE" \
  || fail "capture must expose randomAccessActive()"

grep -q 'randomAccessActive' "$HDR" \
  || fail "LibavCaptureSource must declare randomAccessActive()"

# mpeg2tsRandomAccess must only be emitted behind the randomAccess flag.
grep -q 'if (catalog.randomAccess' "$MUXER" \
  || fail "mpeg2tsRandomAccess must be gated on catalog.randomAccess"

grep -q 'keyframeIntervalMs' "$REPO_ROOT/src/app/PublishConfig.h" \
  || fail "PublishConfig must expose keyframeIntervalMs"

grep -q 'gop_size = gopSize' "$CAPTURE" \
  || fail "encoder must set a bounded gop_size for periodic IDRs"

printf 'keyframe-aligned-groups guards passed\n'
