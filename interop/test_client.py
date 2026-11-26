import contextlib
import io
import signal
import subprocess
import sys
import threading
import time
import unittest

import client
import tsfile

SCENARIOS = ['ts-carriage-integrity', 'ts-late-subscriber', 'ts-clock-step', 'ts-multi-program',
             'ts-publisher-restart']


def packet(pid, rap=False, fill=0xFF):
    head = bytes([0x47, (0x40 if rap else 0) | (pid >> 8), pid & 0xFF, 0x30 if rap else 0x10])
    body = bytes([1, 0x40]) if rap else b''
    return head + body + bytes([fill]) * (188 - len(head) - len(body))


def run_main(argv, environ, scenarios=None):
    out = io.StringIO()
    with contextlib.redirect_stdout(out), contextlib.redirect_stderr(io.StringIO()):
        code = client.main(argv, environ, scenarios)
    return code, out.getvalue()


def passing(options):
    return client.Result(options.test_id, [client.Check('ordering', True, 'in order')], {'duration_ms': 1})


class TsFileTest(unittest.TestCase):
    def test_first_random_access_is_on_a_video_pid_at_or_after_start(self):
        packets = [packet(0), packet(0x101, rap=True), packet(0x100), packet(0x100, rap=True), packet(0x311, rap=True)]
        self.assertEqual(tsfile.first_random_access(packets), 3)
        self.assertEqual(tsfile.first_random_access(packets, 4), 4)
        with self.assertRaises(ValueError):
            tsfile.first_random_access(packets[:3])

    def test_paced_chunks_follow_the_constant_rate(self):
        data = b''.join(packet(0x100, fill=i) for i in range(10))
        chunks = list(tsfile.paced_chunks(data, 20.0, 3, chunk_packets=4))
        self.assertEqual([round(due, 3) for due, _ in chunks], [6.0, 14.0])
        self.assertEqual(b''.join(chunk for _, chunk in chunks), data[3 * 188:])


class CheckerTest(unittest.TestCase):
    OUTPUT = ('TAP version 14\n1..3\n'
              'ok 1 - catalog # SKIP MPEG-TS has no description track of its own\n'
              'ok 2 - byte-identical (the fixture, from packet 0)\n'
              'not ok 3 - group-starts (group 4 starts late (at packet 9))\n'
              '# media_format: "mpeg-ts"\n# ts_packets_received: 7\n')

    def test_results_and_diagnostics(self):
        checks = client.parse_checker(self.OUTPUT, 1)
        self.assertEqual(checks, [
            client.Check('catalog', None, 'MPEG-TS has no description track of its own'),
            client.Check('byte-identical', True, 'the fixture, from packet 0'),
            client.Check('group-starts', False, 'group 4 starts late (at packet 9)')])
        self.assertEqual(client.parse_diagnostics(self.OUTPUT),
                         {'media_format': 'mpeg-ts', 'ts_packets_received': 7})

    def test_a_checker_that_says_nothing_fails(self):
        checks = client.parse_checker('Traceback (most recent call last):\n', 1)
        self.assertEqual([c.ok for c in checks], [False])

    def test_a_checker_that_dies_after_passing_lines_fails(self):
        checks = client.parse_checker('ok 1 - ordering (fine)\n', 2)
        self.assertIn(False, [c.ok for c in checks])

    def test_a_missing_checker_fails(self):
        output, code = client.run_checker(['/nonexistent/check-ts.py'], timeout=5)
        self.assertIn(False, [c.ok for c in client.parse_checker(output, code)])


class TapTest(unittest.TestCase):
    def test_pass_failure_and_skip(self):
        results = [
            client.Result('ts-carriage-integrity', [client.Check('catalog', None, 'no description track'),
                                                    client.Check('ordering', True, 'in order')],
                          {'duration_ms': 5, 'sessions': {'publisher': {'moqt_version': 'moqt-22'}}}),
            client.Result('ts-clock-step', [client.Check('decode', False, 'short by 3 frames')], {}),
        ]
        report = client.tap_report(results, 'moqt://relay:4443')
        self.assertTrue(report.startswith('TAP version 14\n'))
        self.assertIn('\n1..2\n# Subtest: ts-carriage-integrity\n    1..2\n'
                      '    ok 1 - catalog # SKIP no description track\n    ok 2 - ordering (in order)\n'
                      'ok 1 - ts-carriage-integrity\n  ---\n', report)
        self.assertIn('  sessions:\n    publisher:\n      moqt_version: "moqt-22"\n', report)
        self.assertIn('    not ok 1 - decode (short by 3 frames)\nnot ok 2 - ts-clock-step\n', report)
        self.assertIn('  message: "decode: short by 3 frames"\n', report)

    def test_a_scenario_not_run_is_a_skipped_point(self):
        code, out = run_main(['--test', 'ts-clock-step'], {}, {})
        self.assertEqual(code, 0)
        self.assertIn('\nok 1 - ts-clock-step # SKIP not implemented\n', out)

    def test_a_result_with_only_skips_does_not_pass(self):
        result = client.Result('ts-clock-step', [client.Check('catalog', None, 'none')], {})
        self.assertFalse(result.passed)


class MainTest(unittest.TestCase):
    def test_list(self):
        code, out = run_main(['--list'], {})
        self.assertEqual((code, out.split()), (0, SCENARIOS))

    def test_unknown_test_is_127(self):
        self.assertEqual(run_main([], {'TESTCASE': 'announce-only'})[0], 127)
        self.assertEqual(run_main(['--test', 'nope'], {})[0], 127)

    def test_empty_testcase_runs_everything(self):
        seen = []

        def record(options):
            seen.append(options.test_id)
            return passing(options)

        code, _ = run_main([], {'TESTCASE': ''}, {name: record for name in SCENARIOS})
        self.assertEqual((code, seen), (0, SCENARIOS))

    def test_environment_options_and_exit_code(self):
        for value in ('1', 'true'):
            seen = []

            def fail(options):
                seen.append((options.relay, options.insecure))
                return client.Result(options.test_id, [client.Check('decode', False, 'no')], {})

            code, _ = run_main(['--test', 'ts-clock-step'],
                               {'RELAY_URL': 'moqt://r:1', 'TLS_DISABLE_VERIFY': value}, {'ts-clock-step': fail})
            self.assertEqual((code, seen), (1, [('moqt://r:1', True)]))

    def test_scenarios_run_at_once_and_report_in_order(self):
        # Each waits for all the others: only concurrent runs get past this.
        together = threading.Barrier(len(SCENARIOS), timeout=5)

        def meet(options):
            together.wait()
            return passing(options)

        code, out = run_main([], {}, {name: meet for name in SCENARIOS})
        self.assertEqual(code, 0, out)
        self.assertEqual([line.split()[3] for line in out.splitlines() if line.startswith('ok ')], SCENARIOS)

    def test_media_sequential_runs_one_at_a_time(self):
        running, most = [0], [0]
        lock = threading.Lock()

        def count(options):
            with lock:
                running[0] += 1
                most[0] = max(most[0], running[0])
            time.sleep(0.05)
            with lock:
                running[0] -= 1
            return passing(options)

        for value in ('1', 'true'):
            code, _ = run_main([], {'MEDIA_SEQUENTIAL': value}, {name: count for name in SCENARIOS})
            self.assertEqual((code, most[0]), (0, 1))

    def test_a_scenario_that_raises_is_not_ok(self):
        def boom(options):
            raise RuntimeError('relay refused')

        code, out = run_main(['--test', 'ts-clock-step'], {}, {'ts-clock-step': boom})
        self.assertEqual(code, 1)
        self.assertIn('not ok 1 - ts-clock-step', out)
        self.assertIn('relay refused', out)


class RelayUrlTest(unittest.TestCase):
    def relay_seen_by_a_scenario(self, url):
        seen = []

        def record(options):
            seen.append(options.relay)
            return passing(options)

        run_main(['--relay', url, '--test', 'ts-clock-step'], {}, {'ts-clock-step': record})
        return seen[0]

    # Without a path the publisher takes "/" and the receiver "/moq", and
    # they then meet in different places on the relay.
    def test_https_without_a_path_gets_the_root_path(self):
        self.assertEqual(self.relay_seen_by_a_scenario('https://relay:4443'), 'https://relay:4443/')
        self.assertEqual(self.relay_seen_by_a_scenario('https://relay:4443?jwt=a'), 'https://relay:4443/?jwt=a')

    def test_other_urls_are_passed_through(self):
        for url in ('https://relay:4443/', 'https://relay:4443/anon', 'moqt://relay:4443'):
            self.assertEqual(self.relay_seen_by_a_scenario(url), url)


class ChildrenTest(unittest.TestCase):
    def test_a_child_that_ignores_sigterm_is_killed(self):
        script = 'import signal, time; signal.signal(signal.SIGTERM, signal.SIG_IGN); print("up", flush=True); time.sleep(60)'
        started = time.monotonic()
        with client.Children(grace=0.5) as children:
            child = children.spawn([sys.executable, '-c', script], stdout=subprocess.PIPE)
            child.stdout.readline()
        self.assertEqual(child.returncode, -signal.SIGKILL)
        self.assertLess(time.monotonic() - started, 10)
        child.stdout.close()


if __name__ == '__main__':
    unittest.main()
