#!/usr/bin/env python3
"""moq2ts client for the MoQ interop runner's MPEG-TS scenarios.

Follows the runner's docs/TEST-CLIENT-INTERFACE.md and docs/tests/media/:
TAP 14 on stdout, one test point per scenario with a subtest per check.
Exit 0 when every selected scenario passes, 1 on a failure, 127 for a test
this client does not expose. Selected scenarios run at once; MEDIA_SEQUENTIAL=1
runs them one at a time.
"""

import argparse
import concurrent.futures
import contextlib
import json
import os
import re
import subprocess
import sys
import time
from dataclasses import dataclass, field

VERSION = '0.1.0'
SCENARIOS = ('ts-carriage-integrity', 'ts-late-subscriber', 'ts-clock-step', 'ts-multi-program',
             'ts-publisher-restart')


@dataclass(frozen=True)
class Check:
    name: str
    ok: object   # True, False, or None for a check that could not be run
    detail: str


@dataclass
class Result:
    test_id: str
    checks: list
    diag: dict = field(default_factory=dict)

    @property
    def passed(self):
        # A skipped check proves nothing: something must have passed.
        return any(c.ok is True for c in self.checks) and not any(c.ok is False for c in self.checks)

    @property
    def skipped(self):
        return bool(self.checks) and all(c.ok is None for c in self.checks)


@dataclass(frozen=True)
class Options:
    test_id: str
    relay: str
    insecure: bool
    verbose: bool
    environ: dict


class Children(contextlib.AbstractContextManager):
    """Processes that do not outlive the block: terminated, then killed, each by
    its own PID."""

    def __init__(self, grace=5.0):
        self.grace = grace
        self.processes = []

    def spawn(self, argv, **kwargs):
        process = subprocess.Popen(argv, **kwargs)
        self.processes.append(process)
        return process

    def __exit__(self, *exc):
        for process in reversed(self.processes):
            if process.poll() is None:
                process.terminate()
        deadline = time.monotonic() + self.grace
        for process in reversed(self.processes):
            try:
                process.wait(max(0.0, deadline - time.monotonic()))
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
        return False


_RESULT = re.compile(r'^(not ok|ok) \d+ - ([\w-]+)(?:\s+# SKIP\s*(.*)|\s+\((.*)\))?\s*$')


def parse_checker(output, returncode):
    """The checks check-ts.py printed. A checker that printed none, or that
    exited nonzero with none failing, has failed."""
    checks = []
    for line in output.splitlines():
        match = _RESULT.match(line)
        if not match:
            continue
        status, name, skipped, detail = match.groups()
        if skipped is not None:
            checks.append(Check(name, None, skipped))
        else:
            checks.append(Check(name, status == 'ok', detail or ''))
    if not checks or (returncode != 0 and not any(c.ok is False for c in checks)):
        last = output.strip().splitlines()[-1] if output.strip() else 'no output'
        checks.append(Check('checker', False, f'exit {returncode}: {last}'))
    return checks


def parse_diagnostics(output):
    """The "# key: value" lines check-ts.py prints for the YAML block."""
    diag = {}
    for line in output.splitlines():
        match = re.match(r'^# (\w+): (.*)$', line)
        if match:
            try:
                diag[match.group(1)] = json.loads(match.group(2))
            except ValueError:
                diag[match.group(1)] = match.group(2)
    return diag


def run_checker(argv, timeout):
    """(output, exit status) of the checker, run with this Python."""
    try:
        run = subprocess.run([sys.executable, *argv], capture_output=True, text=True, timeout=timeout)
    except (OSError, subprocess.TimeoutExpired) as error:
        return f'{error}\n', 2
    return run.stdout + (run.stderr if run.returncode else ''), run.returncode


def _yaml(mapping, indent):
    lines = []
    for key, value in mapping.items():
        if isinstance(value, dict):
            lines.append(f'{indent}{key}:')
            lines.extend(_yaml(value, indent + '  '))
        else:
            # A JSON scalar or list is YAML too.
            lines.append(f'{indent}{key}: {json.dumps(value)}')
    return lines


def tap_report(results, relay):
    lines = ['TAP version 14', f'# moq2ts-interop-client v{VERSION}', f'# Relay: {relay}', f'1..{len(results)}']
    for number, result in enumerate(results, 1):
        lines.append(f'# Subtest: {result.test_id}')
        lines.append(f'    1..{len(result.checks)}')
        failed = None
        for index, check in enumerate(result.checks, 1):
            detail = ' '.join(check.detail.split())
            if check.ok is None:
                lines.append(f'    ok {index} - {check.name} # SKIP {detail}')
                continue
            status = 'ok' if check.ok else 'not ok'
            lines.append(f'    {status} {index} - {check.name}' + (f' ({detail})' if detail else ''))
            if not check.ok and failed is None:
                failed = f'{check.name}: {detail}'
        diag = dict(result.diag)
        if failed and 'message' not in diag:
            diag['message'] = failed
        if result.skipped:
            lines.append(f'ok {number} - {result.test_id} # SKIP {result.checks[0].detail}')
        else:
            lines.append(f'{"ok" if result.passed else "not ok"} {number} - {result.test_id}')
        lines.append('  ---')
        lines.extend(_yaml(diag, '  '))
        lines.append('  ...')
    return '\n'.join(lines) + '\n'


def main(argv, environ, scenarios=None):
    """scenarios maps a test id to a function of Options that returns a Result."""
    parser = argparse.ArgumentParser(description='moq2ts MPEG-TS interop test client')
    parser.add_argument('-r', '--relay', default=environ.get('RELAY_URL') or 'https://localhost:4443')
    parser.add_argument('-t', '--test', default=environ.get('TESTCASE') or None)
    parser.add_argument('-l', '--list', action='store_true')
    parser.add_argument('-v', '--verbose', action='store_true',
                        default=environ.get('VERBOSE') in ('1', 'true'))
    parser.add_argument('--tls-disable-verify', action='store_true',
                        default=environ.get('TLS_DISABLE_VERIFY') in ('1', 'true'))
    args = parser.parse_args(argv)
    if scenarios is None:
        import scenarios as registry
        scenarios = registry.SCENARIOS
    if args.list:
        print('\n'.join(SCENARIOS))
        return 0
    if args.test and args.test not in SCENARIOS:
        print(f'unsupported test: {args.test}', file=sys.stderr)
        return 127
    selected = [args.test] if args.test else list(SCENARIOS)

    def run_one(test_id):
        options = Options(test_id, args.relay, args.tls_disable_verify, args.verbose, dict(environ))
        started = time.monotonic()
        if test_id not in scenarios:
            result = Result(test_id, [Check(test_id, None, 'not implemented')])
        else:
            try:
                result = scenarios[test_id](options)
            except Exception as error:   # a scenario that breaks is a failed one
                result = Result(test_id, [Check('run', False, f'{type(error).__name__}: {error}')])
        result.diag.setdefault('duration_ms', int((time.monotonic() - started) * 1000))
        return result

    # The scenarios play 20 s streams each, in namespaces of their own: at once
    # they fit the runner's time budget, one after another they do not.
    # MEDIA_SEQUENTIAL runs them one at a time, to tell a fault from load.
    workers = 1 if environ.get('MEDIA_SEQUENTIAL') in ('1', 'true') else len(selected)
    with concurrent.futures.ThreadPoolExecutor(max_workers=workers) as pool:
        results = list(pool.map(run_one, selected))
    sys.stdout.write(tap_report(results, args.relay))
    sys.stdout.flush()
    return 0 if all(r.passed or r.skipped for r in results) else 1


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:], os.environ))
