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
}
DRAFTS = (22, 18)
PACKAGING = 'msfts-mpeg2ts'


def _session_options(options, workdir):
    environ = options.environ
    return session.SessionOptions(
        cli=environ.get('MOQ2TS_CLI', '/app/moq2ts-cli'),
        receiver=environ.get('MOQ2TS_RECEIVER', '/app/moq2ts-receiver'),
        relay=options.relay, insecure=options.insecure, workdir=workdir,
        # The fixtures play for 20 s.
        duration=float(environ.get('MEDIA_FIXTURE_SECONDS', 20)),
        ready_timeout=float(environ.get('MEDIA_READY_TIMEOUT', 5)))


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
        return client.Result(options.test_id, client.parse_checker(output, status), diag)


SCENARIOS = {test_id: functools.partial(carriage, fixture, flags)
             for test_id, (fixture, flags) in CARRIAGE.items()}
