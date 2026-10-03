#!/usr/bin/env bash
set -euo pipefail

# Guards for the transparent/whole-multiplex carriage profile (msfts#7 suggestion 3).
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
CFG="$REPO_ROOT/src/app/PublishConfig.h"
MUXER="$REPO_ROOT/src/media/MsftsMuxer.cpp"
HDR="$REPO_ROOT/src/media/MsftsMuxer.h"
PKT="$REPO_ROOT/src/media/M2tsPacketizer.cpp"
PIPE="$REPO_ROOT/src/media/LivePipeline.cpp"

fail() { printf '%s\n' "$1" >&2; exit 1; }

# Config toggle exists and defaults to the historical filtered behavior.
grep -q 'bool transparentMode = false;' "$CFG" \
  || fail "PublishConfig must carry transparentMode defaulting to false"

# Catalog carries the carriage mode and gates the program fields on it (draft
# field: mpeg2tsMode).
grep -q 'Mpeg2tsMode mode' "$HDR" \
  || fail "MsftsCatalog must carry the mpeg2tsMode value"
grep -q 'catalog.mode == Mpeg2tsMode::UnmodifiedMultiplex' "$MUXER" \
  || fail "program fields must be gated on the unmodified-multiplex mode"
grep -q '"unmodified-multiplex"' "$MUXER" \
  || fail "catalog must emit the draft-conformant unmodified-multiplex mode"
grep -q '"unmodified-program"' "$MUXER" \
  || fail "catalog must emit the draft-conformant unmodified-program mode"
grep -q '"per-program"' "$MUXER" \
  || fail "catalog must emit the draft-conformant per-program mode"
# The legacy m2tsMpts/m2tsTransparent spellings stay accepted on parse for interop.
grep -q '"m2tsMpts"' "$MUXER" \
  || fail "catalogFromJson must still accept the legacy m2tsMpts key"
grep -q '"m2tsTransparent"' "$MUXER" \
  || fail "catalogFromJson must still accept the legacy m2tsTransparent key"
# Per-program identifiers remain available for the non-transparent branch.
grep -q 'mpeg2tsProgramNumber' "$MUXER" \
  || fail "filtered path must still emit mpeg2tsProgramNumber"

# Packetizer bypasses PAT/PMT parse and PID filtering when transparent.
grep -q 'if (!m_transparent)' "$PKT" \
  || fail "packetizer must guard collectInitData/filtering on !m_transparent"
grep -q 'setTransparent' "$PKT" \
  || fail "packetizer must expose setTransparent"

# Pipeline wires the packetizer and catalog for transparent mode.
grep -q 'setTransparent(m_config.transparentMode)' "$PIPE" \
  || fail "pipeline must pass transparentMode into the packetizer"
grep -q 'packetizer.patProgramCount() == 1' "$PIPE" \
  || fail "pipeline must pick the unmodified mode from the PAT program count"

printf 'transparent-mode guards passed\n'
