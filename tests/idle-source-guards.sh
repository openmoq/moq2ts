#!/usr/bin/env bash
# A live source with nothing to give answers none and says it is not finished.
# The two only work as a pair: without is_finished, the first input pause ends
# the publish. No ctest reaches the relay path, so guard its wiring here.
set -euo pipefail
cd "$(dirname "$0")/.."
publisher=src/publish/MoqxrPublisher.cpp
fail() { echo "FAIL: $*" >&2; exit 1; }

grep -q 'state->sourceEnd\.take(lookahead->next())' "$publisher" ||
  fail "the live source does not pass what it reads through LiveSourceEnd"
grep -q 'source\.is_finished = \[state\] { return state->sourceEnd\.finished(); };' "$publisher" ||
  fail "the live source does not set is_finished from LiveSourceEnd"
echo "idle source guards passed"
