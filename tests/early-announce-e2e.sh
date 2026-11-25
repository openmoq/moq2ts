#!/usr/bin/env bash
# moq2ts-cli --announce-early -> relay -> moq2ts-receiver, the receiver
# subscribed before media starts. The publisher gets only the packets before
# the first video packet; once the receiver has the track, the rest follows at
# the mux rate. The capture must hold every non-null packet of the source.
# Exits non-zero otherwise.
# Requires a relay (a local moq5-relay by default, see e2e-relay.sh), ffmpeg,
# and python3.
#
# Environment:
#   RELAY_URL         an existing relay, e.g. moqt://127.0.0.1:4433; without it
#                     a local relay starts from MOQ5_RELAY (see e2e-relay.sh)
#   MOQ2TS_CLI        the moq2ts-cli binary (required)
#   MOQ2TS_RECEIVER   the moq2ts-receiver binary (required)
#   SOURCE_SECONDS    length of the source in seconds (default 20)
#   DRAFT             MOQT draft of the publisher and receiver, 22 or 18 (default 22)
#   NAMESPACE         MOQT namespace (default e2e/early/<time>)
#   INSECURE          1: do not verify the certificate of a RELAY_URL relay
set -euo pipefail

: "${MOQ2TS_CLI:?set MOQ2TS_CLI to the moq2ts-cli binary}"
: "${MOQ2TS_RECEIVER:?set MOQ2TS_RECEIVER to the moq2ts-receiver binary}"
SOURCE_SECONDS="${SOURCE_SECONDS:-20}"
NAMESPACE="${NAMESPACE:-e2e/early/$(date +%s)}"
DRAFT="${DRAFT:-22}"
for tool in ffmpeg python3 timeout; do
  command -v "$tool" > /dev/null || { echo "missing required tool: $tool" >&2; exit 1; }
done

work="$(mktemp -d)"
pids=()
ok=0
cleanup() {
  for pid in "${pids[@]}"; do kill "$pid" 2>/dev/null || true; done
  wait 2>/dev/null || true
  if [ "$ok" -eq 1 ]; then rm -rf "$work"; else echo "artifacts kept in $work" >&2; fi
}
trap cleanup EXIT
# shellcheck source=tests/e2e-relay.sh
. "$(dirname "${BASH_SOURCE[0]}")/e2e-relay.sh"
e2e_relay_start "$work" || exit 1
if [ "${INSECURE:-0}" = 1 ]; then E2E_INSECURE=true; fi

# One program at a constant mux rate, a random access point every second.
timeout 120 ffmpeg -hide_banner -loglevel error -y \
  -f lavfi -i testsrc2=size=640x360:rate=25 -f lavfi -i sine=frequency=1000:sample_rate=48000 \
  -c:v libx264 -preset ultrafast -g 25 -keyint_min 25 -sc_threshold 0 -bf 0 -b:v 1500k \
  -c:a aac -b:a 128k -t "$SOURCE_SECONDS" -muxrate 2000000 -f mpegts "$work/source.ts"

export WORK="$work" NAMESPACE DRAFT SOURCE_SECONDS E2E_INSECURE RELAY_URL MOQ2TS_CLI MOQ2TS_RECEIVER
timeout "$((SOURCE_SECONDS + 90))" python3 - <<'PY'
import os, subprocess, sys, threading, time
env = os.environ
work = env['WORK']
insecure = ['--insecure'] if env.get('E2E_INSECURE') == 'true' else []
data = open(f'{work}/source.ts', 'rb').read()
packets = [data[i:i + 188] for i in range(0, len(data), 188)]
pid = lambda p: ((p[1] & 0x1F) << 8) | p[2]
lead = next(i for i, p in enumerate(packets) if pid(p) == 0x100)

def follow(stream, log, marker, seen):
    for line in iter(stream.readline, b''):
        log.write(line)
        log.flush()
        if marker in line:
            seen.set()

def fail(why):
    print(f'FAIL: {why}', file=sys.stderr)
    for name in ('publisher', 'receiver'):
        path = f'{work}/{name}.log'
        if os.path.exists(path):
            print(f'--- last lines of {name}.log', file=sys.stderr)
            print(b''.join(open(path, 'rb').readlines()[-8:]).decode(errors='replace'), file=sys.stderr)
    for process in processes:
        if process.poll() is None:
            process.kill()
    sys.exit(1)

processes = []
publisher = subprocess.Popen(
    [env['MOQ2TS_CLI'], '--video', '/dev/stdin', '--unmodified', '--announce-early', '--draft', env['DRAFT'],
     '--endpoint', env['RELAY_URL'], '--namespace', env['NAMESPACE']] + insecure,
    stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
processes.append(publisher)
ready = threading.Event()
threading.Thread(target=follow, daemon=True,
                 args=(publisher.stdout, open(f'{work}/publisher.log', 'wb'), b'[ready]', ready)).start()
publisher.stdin.write(b''.join(packets[:lead]))
publisher.stdin.flush()
if not ready.wait(15):
    fail('the publisher was not ready with the leading tables alone')

receiver = subprocess.Popen(
    [env['MOQ2TS_RECEIVER'], '--url', env['RELAY_URL'], '--namespace', env['NAMESPACE'], '--draft', env['DRAFT'],
     '--out', f'{work}/received.ts', '--object-log', f'{work}/objects.txt', '--duration', str(int(env['SOURCE_SECONDS']) + 40)] + insecure,
    stderr=subprocess.PIPE)
processes.append(receiver)
subscribed = threading.Event()
follower = threading.Thread(target=follow, daemon=True,
                            args=(receiver.stderr, open(f'{work}/receiver.log', 'wb'), b'[rx] subscription active', subscribed))
follower.start()
if not subscribed.wait(15):
    fail('the subscription was not active while no media had arrived')
time.sleep(1)

start, sent = time.monotonic(), 0
for i in range(lead, len(packets), 70):
    chunk = b''.join(packets[i:i + 70])
    publisher.stdin.write(chunk)
    publisher.stdin.flush()
    sent += len(chunk)
    delay = start + sent * 8 / 2_000_000 - time.monotonic()
    if delay > 0:
        time.sleep(delay)
publisher.stdin.close()
try:
    status = receiver.wait(30)
except subprocess.TimeoutExpired:
    fail('the receiver did not see the track end')
follower.join(5)
if status != 0:
    fail(f'the receiver exited with {status}')
publisher.wait(15)

received = open(f'{work}/received.ts', 'rb').read()
got = [received[i:i + 188] for i in range(0, len(received), 188)]
carried = lambda stream: [p for p in stream if pid(p) != 0x1FFF]
if carried(got) != carried(packets):
    fail(f'the capture holds {len(carried(got))} of {len(carried(packets))} non-null packets, or others')
objects = [line.split() for line in open(f'{work}/objects.txt')]
if sum(int(o[2]) for o in objects) != len(received):
    fail('the object log does not add up to the capture')
groups = [int(o[0]) for o in objects]
if groups != sorted(groups):
    fail('the object log has a Group ID that goes back')
print(f'OK: draft {env["DRAFT"]}: all {len(carried(packets))} non-null packets, subscribed before media')
PY
ok=1
