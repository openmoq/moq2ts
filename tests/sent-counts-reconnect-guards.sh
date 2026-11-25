#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
PUB="$REPO_ROOT/src/publish/MoqxrPublisher.cpp"

fail() { printf '%s\n' "$1" >&2; exit 1; }

# A connection that fails and reconnects keeps its sent counts: they join
# m_sentEnded* before the next connection starts from 0. Without that, the
# periodic [stats] line (PublishTotals::observeSent ignores a lower reading)
# stays frozen until the new connection has sent more than the failed one,
# and the [final] totals miss the failed connection.
awk '
    /if \(!status\.ok && stillRunning && !publisherStopped\)/ { branch = NR }
    branch && !folded && /m_sentEndedObjects \+=/ { folded = NR }
    branch && !folded && /prepareReconnect\(/ { exit 1 }
    END { exit !(branch && folded) }
' "$PUB" || fail "the failure branch must add the failed connection's sent counts to m_sentEnded* before it reconnects"

# The count moves in the same locked step that drops m_activePublisher, so
# sentSoFar() never counts the failed connection twice or not at all.
awk '
    /if \(!status\.ok && stillRunning && !publisherStopped\)/ { branch = NR }
    branch && /QMutexLocker locker\(&m_mutex\);/ && !lock { lock = NR }
    branch && /m_sentEndedObjects \+=/ && !add { add = NR }
    branch && /m_activePublisher\.reset\(\);/ && !reset { reset = NR }
    branch && /locker\.unlock\(\);/ && !unlock { unlock = NR }
    END { exit !(lock && add && reset && unlock && lock < add && add < unlock && lock < reset && reset < unlock) }
' "$PUB" || fail "the failed connection's counts must move to m_sentEnded* under the lock that drops m_activePublisher"

printf 'sent-counts reconnect guards passed\n'
