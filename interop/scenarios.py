"""The scenarios this client runs: test id to a function of client.Options that
returns a client.Result. A listed id missing here is reported as skipped."""

import functools
import os
import secrets
import tempfile
from pathlib import Path

import client
import session

# test id: (fixture, check-ts.py flags)
CARRIAGE = {
    'ts-carriage-integrity': ('fixture.ts', []),
    'ts-clock-step': ('fixture-step.ts', ['--expect-step']),
    'ts-multi-program': ('fixture-mpts.ts', ['--programs', '3']),
    'ts-late-subscriber': ('fixture.ts', ['--late-join']),
}
DRAFTS = (22, 18)
PACKAGING = 'msfts-mpeg2ts'


def join_point(first_group, join_group, join_group_ok):
    """ts-late-subscriber: the lowest Group received is at least the one open at
    SUBSCRIBE, and at most the one after the one open at SUBSCRIBE_OK."""
    if first_group is None or join_group is None or join_group_ok is None:
        return client.Check('join-point', False,
                            f'unknown: first group {first_group}, open at the join {join_group}/{join_group_ok}')
    return client.Check('join-point', join_group <= first_group <= join_group_ok + 1,
                        f'first group {first_group}, {join_group} open at SUBSCRIBE, {join_group_ok} when accepted')


def _first_group(objects):
    groups = [int(line.split()[0]) for line in objects.read_text().splitlines() if line.split()]
    return min(groups, default=None)


def _session_options(options, workdir):
    environ = options.environ
    return session.SessionOptions(
        cli=environ.get('MOQ2TS_CLI', '/app/moq2ts-cli'),
        receiver=environ.get('MOQ2TS_RECEIVER', '/app/moq2ts-receiver'),
        relay=options.relay, insecure=options.insecure, workdir=workdir,
        # The fixtures play for 20 s.
        duration=float(environ.get('MEDIA_FIXTURE_SECONDS', 20)),
        ready_timeout=float(environ.get('MEDIA_READY_TIMEOUT', 5)),
        join_at=float(environ.get('MEDIA_JOIN_AT', 8.5)) if options.test_id == 'ts-late-subscriber' else None,
        settle=float(environ.get('MEDIA_SETTLE', 3)))


def carriage(fixture_name, flags, options):
    environ = options.environ
    run_id = environ.get('MEDIA_RUN_ID') or secrets.token_hex(16)
    namespace = f'moq-test/{options.test_id}/{run_id}'
    fixture = Path(environ.get('MEDIA_FIXTURE_DIR', '/app/fixture')) / fixture_name
    checker = environ.get('MEDIA_TS_CHECKER', '/app/check-ts.py')
    transport = 'webtransport' if options.relay.startswith('https://') else 'quic'
    with tempfile.TemporaryDirectory() as temporary:
        for draft in DRAFTS:
            outcome = session.run(fixture, namespace, draft,
                                  _session_options(options, Path(temporary) / f'draft{draft}'))
            if outcome.error != session.NOT_READY:
                break
        version = {'moqt_version': f'moqt-{draft}', 'transport': transport}
        diag = {'media_format': 'mpeg-ts', 'ts_packaging': PACKAGING, 'run_id': run_id, 'namespace': namespace,
                'sessions': {'publisher': version, 'subscriber': dict(version)}}
        if outcome.error:
            detail = f'{outcome.error}; {outcome.log_tail()}'
            return client.Result(options.test_id, [client.Check('delivery', False, detail)], diag)
        output, status = client.run_checker(
            [checker, '--fixture', str(fixture), '--received', str(outcome.received),
             '--objects', str(outcome.objects), *flags], timeout=60)
        if options.verbose:
            print(output, end='', file=os.sys.stderr)
        diag.update(client.parse_diagnostics(output))
        checks = client.parse_checker(output, status)
        if options.test_id == 'ts-late-subscriber':
            checks.append(join_point(_first_group(outcome.objects), outcome.join_group, outcome.join_group_ok))
            diag.update(join_group=outcome.join_group, join_group_ok=outcome.join_group_ok,
                        join_offset_ms=outcome.join_offset_ms)
        return client.Result(options.test_id, checks, diag)


SCENARIOS = {test_id: functools.partial(carriage, fixture, flags)
             for test_id, (fixture, flags) in CARRIAGE.items()}
