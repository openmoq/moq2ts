#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
CLI="$REPO_ROOT/src/cli/main_cli.cpp"
REDUNDANT_H="$REPO_ROOT/src/publish/RedundantPublisher.h"
PUBLISHER_CPP="$REPO_ROOT/src/publish/MoqxrPublisher.cpp"

fail() { printf '%s\n' "$1" >&2; exit 1; }

# --endpoint may be given several times.
grep -q 'parser.values(QStringLiteral("endpoint"))' "$CLI" \
  || fail "the CLI must read every --endpoint value"

# One endpoint keeps the old code path: no endpoint list, no composite, and
# the MoqxrPublisher publishes alone.
grep -q 'if (endpoints.size() > 1) {' "$CLI" \
  || fail "the CLI must fill the endpoint list only for several endpoints"
grep -q 'static bool needed(const PublishConfig& cfg) { return cfg.moqEndpoints.size() > 1; }' "$REDUNDANT_H" \
  || fail "RedundantPublisher::needed must be true only for several endpoints"
grep -q 'if (!moq2ts::RedundantPublisher::needed(cfg)) {' "$CLI" \
  || fail "the CLI must make a RedundantPublisher only when it is needed"
[ "$(grep -c 'redundant ? static_cast<moq2ts::IMoqOutput\*>(redundant.get()) : &publisher;' "$CLI")" -eq 2 ] \
  || fail "both CLI paths must publish through the MoqxrPublisher when there is one relay"

# The connection-start hook does nothing unless a RedundantPublisher set it.
grep -q 'if (m_connectionStartHook) {' "$PUBLISHER_CPP" \
  || fail "MoqxrPublisher must call the connection-start hook only when it is set"

printf 'redundant relays guards passed\n'
