import json
import os
import stat
import sys
import tempfile
import textwrap
import time
import unittest
from pathlib import Path

import client
import scenarios
import session
from test_client import packet

# Stand-ins for the two binaries and the checker. Each appends what it saw to
# events.jsonl in $STUB_DIR.
COMMON = '''
import json, os, sys, time
DIR = os.environ['STUB_DIR']
def event(**fields):
    with open(f'{DIR}/events.jsonl', 'a') as f:
        f.write(json.dumps(fields) + '\\n')
def arg(name):
    return sys.argv[sys.argv.index(name) + 1]
MODE = os.environ.get('STUB_MODE', '')
'''

PUBLISHER = COMMON + '''
event(who='pub', argv=sys.argv[1:])
if MODE == 'only18' and arg('--draft') == '22':
    time.sleep(60)
lead = int(os.environ['STUB_LEAD'])
got = sys.stdin.buffer.read(lead * 188)
event(who='pub', lead=len(got))
print('[ready] namespace accepted', flush=True)
first = sys.stdin.buffer.read(188)
event(who='pub', media_after_active=os.path.exists(f'{DIR}/active'))
rest = first + sys.stdin.buffer.read()
event(who='pub', total=len(got) + len(rest))
open(f'{DIR}/eof', 'w').close()
'''

RECEIVER = COMMON + '''
event(who='rx', argv=sys.argv[1:])
time.sleep(0.2)
open(f'{DIR}/active', 'w').close()
print('[rx] subscription active', file=sys.stderr, flush=True)
while MODE == 'hang' or not os.path.exists(f'{DIR}/eof'):
    time.sleep(0.05)
if MODE != 'nothing':
    open(arg('--out'), 'wb').write(b'x' * 188)
    open(arg('--object-log'), 'w').write('0 0 188\\n')
'''

CHECKER = COMMON + '''
event(who='checker', argv=sys.argv[1:])
print('ok 1 - catalog # SKIP no description track')
print('ok 2 - ordering (in order)')
print('# objects_received: 1')
'''


def stub(directory, name, source):
    path = directory / name
    path.write_text(f'#!{sys.executable}\n' + textwrap.dedent(source))
    path.chmod(path.stat().st_mode | stat.S_IXUSR)
    return path


class SessionTest(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.dir = Path(temporary.name)
        # Three packets, then a random access point, then 96 more.
        data = b''.join([packet(0x11), packet(0), packet(0x1000), packet(0x100, rap=True)] + [packet(0x100)] * 96)
        self.fixtures = self.dir / 'fixtures'
        self.fixtures.mkdir()
        for name in ('fixture.ts', 'fixture-step.ts', 'fixture-mpts.ts'):
            (self.fixtures / name).write_bytes(data)
        self.size = len(data)
        os.environ.update(STUB_DIR=str(self.dir), STUB_LEAD='3', STUB_MODE='')
        self.environ = {
            'MOQ2TS_CLI': str(stub(self.dir, 'cli', PUBLISHER)),
            'MOQ2TS_RECEIVER': str(stub(self.dir, 'rx', RECEIVER)),
            'MEDIA_TS_CHECKER': str(stub(self.dir, 'check', CHECKER)),
            'MEDIA_FIXTURE_DIR': str(self.fixtures),
            'MEDIA_RUN_ID': 'run1',
            'MEDIA_FIXTURE_SECONDS': '0.5',
        }

    def events(self):
        return [json.loads(line) for line in (self.dir / 'events.jsonl').read_text().splitlines()]

    def run_session(self, **overrides):
        options = session.SessionOptions(
            cli=self.environ['MOQ2TS_CLI'], receiver=self.environ['MOQ2TS_RECEIVER'], relay='moqt://r:1',
            insecure=True, workdir=self.dir / 'work', duration=0.5, ready_timeout=3, end_timeout=3)
        for key, value in overrides.items():
            setattr(options, key, value)
        return session.run(self.fixtures / 'fixture.ts', 'moq-test/t/1', 22, options)

    def test_order_and_arguments(self):
        result = self.run_session()
        self.assertIsNone(result.error)
        events = self.events()
        publisher = next(e['argv'] for e in events if e['who'] == 'pub')
        receiver = next(e['argv'] for e in events if e['who'] == 'rx')
        for flag in ('--unmodified', '--announce-early', '--log-groups', '--insecure'):
            self.assertIn(flag, publisher)
        self.assertEqual(publisher[publisher.index('--endpoint') + 1], 'moqt://r:1')
        self.assertEqual(receiver[receiver.index('--object-log') + 1], str(result.objects))
        self.assertIn({'who': 'pub', 'lead': 3 * 188}, events)
        self.assertIn({'who': 'pub', 'media_after_active': True}, events)
        self.assertIn({'who': 'pub', 'total': self.size}, events)
        self.assertEqual(result.received.read_bytes(), b'x' * 188)

    def test_a_receiver_that_never_ends_is_stopped(self):
        os.environ['STUB_MODE'] = 'hang'
        started = time.monotonic()
        result = self.run_session(end_timeout=1)
        self.assertIn('did not end', result.error)
        self.assertLess(time.monotonic() - started, 15)

    def test_a_receiver_that_writes_nothing_is_an_error(self):
        os.environ['STUB_MODE'] = 'nothing'
        self.assertIn('no capture', self.run_session().error)

    def test_a_publisher_that_is_never_ready_is_an_error(self):
        os.environ['STUB_MODE'] = 'only18'
        result = self.run_session(ready_timeout=1)
        self.assertEqual(result.error, session.NOT_READY)

    def test_the_log_tail_keeps_the_error_lines(self):
        log = self.dir / 'publisher.log'
        log.write_text('[error] connect refused\n' + 'noise\n' * 9 + 'last\n')
        result = session.SessionResult(self.dir / 'r', self.dir / 'o', log, self.dir / 'none')
        self.assertEqual(result.log_tail(2), 'publisher: [error] connect refused | publisher: noise | publisher: last')

    def options(self, test_id):
        return client.Options(test_id, 'moqt://r:1', False, False, dict(self.environ))

    def test_scenarios_pass_the_checker_their_flags(self):
        for test_id, fixture, flags in (('ts-carriage-integrity', 'fixture.ts', []),
                                        ('ts-clock-step', 'fixture-step.ts', ['--expect-step']),
                                        ('ts-multi-program', 'fixture-mpts.ts', ['--programs', '3'])):
            (self.dir / 'events.jsonl').unlink(missing_ok=True)
            for marker in ('active', 'eof'):
                (self.dir / marker).unlink(missing_ok=True)
            result = scenarios.SCENARIOS[test_id](self.options(test_id))
            self.assertTrue(result.passed, result.checks)
            checker = next(e['argv'] for e in self.events() if e['who'] == 'checker')
            self.assertEqual(checker[checker.index('--fixture') + 1], str(self.fixtures / fixture))
            self.assertEqual(checker[checker.index('--objects') + 2:], flags)
            self.assertEqual(result.diag['namespace'], f'moq-test/{test_id}/run1')
            self.assertEqual(result.diag['ts_packaging'], 'msfts-mpeg2ts')
            self.assertEqual(result.diag['sessions']['publisher'], {'moqt_version': 'moqt-22', 'transport': 'quic'})
            self.assertEqual(result.diag['objects_received'], 1)

    def test_draft_18_is_tried_when_22_is_not_accepted(self):
        os.environ['STUB_MODE'] = 'only18'
        self.environ['MEDIA_READY_TIMEOUT'] = '1'
        result = scenarios.SCENARIOS['ts-carriage-integrity'](self.options('ts-carriage-integrity'))
        self.assertTrue(result.passed, result.checks)
        self.assertEqual(result.diag['sessions']['publisher']['moqt_version'], 'moqt-18')

    def test_a_failed_session_names_the_reason_and_does_not_run_the_checker(self):
        os.environ['STUB_MODE'] = 'nothing'
        result = scenarios.SCENARIOS['ts-carriage-integrity'](self.options('ts-carriage-integrity'))
        self.assertFalse(result.passed)
        self.assertIn('no capture', result.checks[0].detail)
        self.assertNotIn('checker', [e['who'] for e in self.events()])


if __name__ == '__main__':
    unittest.main()
