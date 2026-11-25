"""One publisher and one receiver through the relay, in the order the runner's
MPEG-TS scenarios give: the subscriber subscribes before media starts, then the
fixture goes out at its own rate."""

import re
import subprocess
import threading
import time
from dataclasses import dataclass
from pathlib import Path

import client
import tsfile

NOT_READY = 'the publisher was not ready'


@dataclass
class SessionOptions:
    cli: str
    receiver: str
    relay: str
    insecure: bool
    workdir: Path
    duration: float = 20.0        # seconds of the fixture
    ready_timeout: float = 5.0
    end_timeout: float = 10.0     # after the fixture's end: T0 + 30 s
    # A late subscriber: the track is subscribed this long after T0, and media
    # starts without waiting for it. T0 is `settle` after the receiver starts.
    join_at: object = None
    settle: float = 3.0


@dataclass
class SessionResult:
    received: Path
    objects: Path
    publisher_log: Path
    receiver_log: Path
    error: object = None   # why the session did not run to its end, or None
    # For a late subscriber: the highest media Group the publisher had opened
    # when SUBSCRIBE went out and when it was accepted, and when it went out.
    join_group: object = None
    join_group_ok: object = None
    join_offset_ms: object = None

    def log_tail(self, lines=3):
        """The error lines and the last lines of both logs, on one line."""
        tail = []
        for name, path in (('publisher', self.publisher_log), ('receiver', self.receiver_log)):
            if path.exists():
                text = path.read_text(errors='replace').strip().splitlines()
                errors = [line for line in text[:-lines] if line.startswith('[error]')]
                tail.extend(f'{name}: {line}' for line in errors[:2] + text[-lines:])
        return ' | '.join(tail)


class _Follower:
    """Copies a process's output to a log, and notes when a marker appears."""

    def __init__(self, stream, log, marker, on_line=None):
        self.seen = threading.Event()
        self.on_line = on_line
        self.thread = threading.Thread(target=self._run, args=(stream, log, marker.encode()), daemon=True)
        self.thread.start()

    def _run(self, stream, log, marker):
        with open(log, 'wb') as out:
            for line in iter(stream.readline, b''):
                out.write(line)
                out.flush()
                if self.on_line:
                    self.on_line(line.decode(errors='replace').strip())
                if marker in line:
                    self.seen.set()
        stream.close()

    def wait(self, process, timeout):
        """True once the marker is seen; False at the timeout or if the process ends first."""
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if self.seen.wait(0.05):
                return True
            if process.poll() is not None:
                self.thread.join(1)
                return self.seen.is_set()
        return False


_GROUP = re.compile(r'^\[group\] track=(\S+) id=(\d+)$')


def run(fixture, namespace, draft, options):
    workdir = Path(options.workdir)
    workdir.mkdir(parents=True, exist_ok=True)
    result = SessionResult(workdir / 'received.ts', workdir / 'objects.txt',
                           workdir / 'publisher.log', workdir / 'receiver.log')
    for path in (result.received, result.objects):
        path.unlink(missing_ok=True)
    data = Path(fixture).read_bytes()
    lead = tsfile.first_random_access(tsfile.packets(data))
    insecure = ['--insecure'] if options.insecure else []
    late = options.join_at is not None
    subscribe_after = ['--subscribe-after', f'{options.settle + options.join_at:g}'] if late else []
    opened = [None]   # the highest media Group the publisher has opened
    sent_at = [None]

    def on_publisher_line(line):
        match = _GROUP.match(line)
        if match and not match.group(1).endswith('.timeline'):
            opened[0] = int(match.group(2))

    def on_receiver_line(line):
        if line == '[rx] subscribe sent':
            result.join_group, sent_at[0] = opened[0], time.monotonic()
        elif line == '[rx] subscription active':
            result.join_group_ok = opened[0]

    with client.Children() as children:
        publisher = children.spawn(
            [options.cli, '--video', '/dev/stdin', '--unmodified', '--announce-early', '--log-groups',
             '--draft', str(draft), '--endpoint', options.relay, '--namespace', namespace, *insecure],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        ready = _Follower(publisher.stdout, result.publisher_log, '[ready]', on_publisher_line)
        # A write to a publisher that stops reading blocks: end it, so that the
        # write fails instead.
        budget = options.ready_timeout + 10 + options.settle * late + options.duration + options.end_timeout
        watchdog = threading.Timer(budget, publisher.kill)
        watchdog.daemon = True
        watchdog.start()
        try:
            try:
                publisher.stdin.write(data[:lead * tsfile.PACKET])
                publisher.stdin.flush()
                if not ready.wait(publisher, options.ready_timeout):
                    result.error = NOT_READY
                    return result
                receiver = children.spawn(
                    [options.receiver, '--url', options.relay, '--namespace', namespace, '--draft', str(draft),
                     '--out', str(result.received), '--object-log', str(result.objects),
                     '--duration', str(int(budget) + 1), *subscribe_after, *insecure],
                    stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
                spawned = time.monotonic()
                # A late subscriber has the catalog before media starts, and
                # subscribes to the track later.
                marker = '[rx] draft=' if late else '[rx] subscription active'
                follower = _Follower(receiver.stderr, result.receiver_log, marker, on_receiver_line)
                if not follower.wait(receiver, 10):
                    result.error = ('the receiver did not get the catalog' if late
                                    else 'the subscription did not become active')
                    return result
                if late:
                    time.sleep(max(0.0, spawned + options.settle - time.monotonic()))

                chunks = tsfile.paced_chunks(data, options.duration, lead)
                start = time.monotonic() - lead * tsfile.PACKET / len(data) * options.duration
                for due, chunk in chunks:
                    delay = start + due - time.monotonic()
                    if delay > 0:
                        time.sleep(delay)
                    publisher.stdin.write(chunk)
                    publisher.stdin.flush()
                publisher.stdin.close()
            except (BrokenPipeError, ValueError):
                result.error = 'the publisher stopped reading its input'
                return result
            try:
                receiver.wait(max(0.0, start + options.duration + options.end_timeout - time.monotonic()))
            except subprocess.TimeoutExpired:
                result.error = 'the receiver did not end by the deadline'
                return result
            follower.thread.join(2)
            if sent_at[0] is not None:
                result.join_offset_ms = int((sent_at[0] - start) * 1000)
            if not result.received.exists() or result.received.stat().st_size == 0:
                result.error = f'no capture (receiver exit {receiver.returncode})'
        finally:
            watchdog.cancel()
            if not publisher.stdin.closed:
                try:
                    publisher.stdin.close()
                except BrokenPipeError:
                    pass
    return result
