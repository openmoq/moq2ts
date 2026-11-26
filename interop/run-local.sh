#!/usr/bin/env bash
# Runs the interop client against a local moq-dev moq-relay on loopback.
# Environment:
#   RELAY_BIN          the moq-relay binary (required)
#   RELAY_PORT         its UDP port (default 4443)
#   MOQ2TS_CLI, MOQ2TS_RECEIVER, MEDIA_FIXTURE_DIR, MEDIA_TS_CHECKER, TESTCASE
#                      as the client reads them
set -euo pipefail
: "${RELAY_BIN:?set RELAY_BIN to the moq-relay binary}"
port="${RELAY_PORT:-4443}"
"$RELAY_BIN" --listen "127.0.0.1:$port" --listen-tls-generate localhost --auth-public '' --log-level error &
relay=$!
trap 'kill "$relay" 2>/dev/null || true; wait "$relay" 2>/dev/null || true' EXIT
sleep 1
kill -0 "$relay"
RELAY_URL="moqt://127.0.0.1:$port" TLS_DISABLE_VERIFY=1 python3 "$(dirname "${BASH_SOURCE[0]}")/client.py" "$@"
