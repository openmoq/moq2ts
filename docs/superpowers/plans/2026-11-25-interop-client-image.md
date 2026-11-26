# moq2ts interop client image Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A container image that runs the runner's five MPEG-TS scenarios with `moq2ts-cli` and `moq2ts-receiver` against `RELAY_URL`, reports TAP 14, and is built and published by moq2ts CI.

**Architecture:** A standard-library Python client in `interop/` drives the two binaries, grades each capture with the runner's `check-ts.py`, and prints TAP. The image builds the binaries, bakes the runner's fixtures and checker from a pinned runner commit, and runs the client.

**Tech Stack:** Python 3 (stdlib, `unittest`), Docker, Ubuntu 24.04, moqxr v0.4.25 SDK, moq5, ffmpeg, TSDuck 3.45-4798, GitHub Actions.

**Spec:** `docs/superpowers/specs/2026-11-25-interop-client-design.md` (approved). The publisher and receiver changes it called for are merged (PRs #82, #83, #84).

**Runner contract** (read at `t0ms/moq-interop-runner` `d2fbb0221c70aca01b42754b9924c007c8cb26e1`, englishm/moq-interop-runner#209, open): `docs/TEST-CLIENT-INTERFACE.md`, `docs/tests/media/README.md`, `docs/tests/media/mpeg-ts.md`, the five `docs/tests/media/ts-*.md`, and `builds/moq-net-media-ts/` as the reference client.

## Global Constraints

- Scenario ids: `ts-carriage-integrity`, `ts-late-subscriber`, `ts-clock-step`, `ts-multi-program`, `ts-publisher-restart`.
- Environment: `RELAY_URL`, `TESTCASE` (empty means all), `TLS_DISABLE_VERIFY` (`1` or `true`), `VERBOSE`, `MEDIA_FIXTURE_DIR` (default `/app/fixture`), `MEDIA_TS_CHECKER` (default `/app/check-ts.py`), `MEDIA_RUN_ID`. Arguments `--relay`, `--test`, `--list`, `--tls-disable-verify`, `-v`.
- Exit 0 all passed, 1 a failure, 127 a test not exposed.
- Namespace `moq-test/<test-id>/<run-id>`.
- TAP YAML: `media_format: "mpeg-ts"`, `ts_packaging: "msfts-mpeg2ts"`, `duration_ms`, `run_id`, `namespace`, `sessions` (`moqt_version`, `transport`), plus the fields `check-ts.py` prints.
- A check that cannot be run is reported `# SKIP`, never passed. `catalog` is skipped.
- No change to the runner's checks or fixtures.
- Drafts 22 and 18 only.
- One client run has 120 s (`TEST_TIMEOUT`) for all scenarios.
- Workflows: `actions/checkout@v5`, `actions/upload-artifact@v5`, `runs-on: ubuntu-24.04`, no third-party actions for what a shell step does.
- Runner scripts are third-party code: they run inside the image build and container only.
- No emoji in commit messages; no co-author or generator lines.

## Review Focus

- The relay is unreachable or refuses the namespace: every selected scenario reports `not ok` with the reason, inside the time budget, exit 1. (Task 2, 3)
- A child process hangs: it is killed by PID at the scenario deadline and none is left behind. (Task 2)
- The checker crashes or prints no result: `not ok`, never passed. (Task 2)
- `TESTCASE=` empty, as the runner's Makefile passes it: runs everything. (Task 2)
- The receiver exits with no capture: the scenario fails with its last log lines in `message`. (Task 3)

---

### Task 1: `--initial-group-id` for a restarted publisher

The checker splits a restarted capture where Group IDs reach the second part's first group (`check_ordering_parts`), so the second publisher's Group IDs must be above the first's.

**Files:** Modify `src/app/PublishConfig.h`, `src/media/M2tsPacketizer.{h,cpp}`, `src/media/LivePipeline.cpp`, `src/cli/main_cli.cpp`, `docs/CLI.md`; test `tests/m2ts_packetizer_test.cpp`.

**Interfaces:** Produces `PublishConfig::initialGroupId` (`std::uint64_t`, default 0), `void M2tsPacketizer::setInitialGroupId(std::uint64_t)` (before `open()`), CLI `--initial-group-id N` (0 to 2^62-1, unmodified or per-program, not `--config`).

- [ ] **Step 1: Failing tests:** with initial id 100, the first Object is Group 100 Object 0 and the next access point is Group 101; the media timeline records carry the same ids; a bad value exits 2.
- [ ] **Step 2: Run, see them fail.**
- [ ] **Step 3: Implement** as the packetizer's starting `m_currentGroupId`, so media, timeline and `[group]` lines agree. The catalog track's numbering is unchanged.
- [ ] **Step 4: All tests and source guards pass.**
- [ ] **Step 5: Commit** `git commit -m "Add --initial-group-id"`

### Task 2: Client core and TAP

**Files:** Create `interop/client.py` (arguments, scenario table, TAP, process handling), `interop/tsfile.py` (fixture reading, lead index, pacing), `interop/test_client.py`; modify `.github/workflows/build.yml` (Linux job runs `python3 -m unittest discover -s interop`).

**Interfaces:**
- `tsfile.first_random_access(packets: list[bytes], start: int = 0) -> int`: first packet at or after `start` on a video PID (0x100, 0x211, 0x311) with `random_access_indicator`.
- `tsfile.paced_chunks(data: bytes, duration_s: float, start: int, chunk_packets: int = 40) -> Iterator[tuple[float, bytes]]`: `(seconds after T0, chunk)` at the fixture's constant rate, so packet `i` is due at `i / total * duration`.
- `client.parse_checker(output: str, returncode: int) -> list[Check]` with `Check(name: str, ok: bool | None, detail: str)`; `ok is None` is a skip. No result lines, or a nonzero exit with none failing, yields a failing check.
- `client.tap_report(results: list[Result]) -> str`, nested subtests and YAML block as in `docs/tests/media/README.md`.
- `client.main(argv, environ) -> int`.

- [ ] **Step 1: Failing unit tests** for each interface: the TAP of a pass, a failure and a skipped check; `--list` prints the five ids; unknown `TESTCASE` returns 127; empty `TESTCASE` selects all; `TLS_DISABLE_VERIFY` of `1` and `true`; a checker that crashes; a child that ignores SIGTERM is killed by PID.
- [ ] **Step 2: Run, see them fail** (`python3 -m unittest discover -s interop -v`).
- [ ] **Step 3: Implement.** All children are started through one context manager that terminates, then kills, by PID.
- [ ] **Step 4: Tests pass.**
- [ ] **Step 5: Commit** `git commit -m "Add the interop client core and TAP output"`

### Task 3: Carriage integrity, clock step, multi-program

**Files:** Create `interop/session.py` (one publisher, one receiver); modify `interop/client.py`; create `interop/run-local.sh` (starts a relay given by `RELAY_BIN`, runs the client with binaries and fixtures from the environment).

**Interfaces:** `session.run(fixture: Path, namespace: str, draft: int, options: SessionOptions) -> SessionResult` with fields `received: Path`, `objects: Path`, `publisher_log`, `receiver_log`, `error: str | None`.

Flow, proven by a local prototype that passed these three: start `moq2ts-cli --video /dev/stdin --unmodified --announce-early --log-groups --draft D`; write the packets before `first_random_access`; wait for `[ready]`; start the receiver with `--object-log`; wait for `[rx] subscription active`; pace the rest; close stdin; wait for the receiver to exit by T0 + 30 s; run `check-ts.py` (`--expect-step`, `--programs 3`). Binaries from `MOQ2TS_CLI` and `MOQ2TS_RECEIVER`. Try draft 22; if the publisher is not ready within 5 s, try 18; the draft used is reported.

- [ ] **Step 1: Failing test:** `interop/test_session.py` with stub publisher and receiver scripts: the order of operations, the arguments, the deadline, a receiver that writes nothing.
- [ ] **Step 2: See it fail. Step 3: Implement. Step 4: Tests pass.**
- [ ] **Step 5: Verify against a real relay and the real checker** (in the checker container, `--network host` for the relay only): all three scenarios, both drafts, every check ok but `catalog` skipped.
- [ ] **Step 6: Commit** `git commit -m "Run the carriage, clock step and multi-program scenarios"`

### Task 4: Late subscriber

Not yet tried. **First step is a throwaway run** to confirm the capture passes `check-ts.py --late-join`; if it does not, stop and revise the spec.

Flow: receiver launched, T0 set 3 s later, `--subscribe-after 11.5` (T0 + 8.5 s); publisher paced from T0 whether or not anyone subscribes to the media. G is the highest `[group]` id printed when the receiver prints `[rx] subscribe sent`, G' when it prints `[rx] subscription active`; the `join-point` subtest passes when `G <= first_group <= G' + 1`. YAML `join_group`, `join_group_ok`, `join_offset_ms`.

- [ ] Throwaway run, then failing test for `join_point(first_group, g, g_ok) -> Check`, implement, verify on both drafts, commit `git commit -m "Run the late subscriber scenario"`.

### Task 5: Publisher restart

Not yet tried. **First step is a throwaway run**; if it does not pass, stop and revise the spec.

Flow: publisher and receiver as Task 3, publisher with `--quic-idle-timeout 2000`. At T0 + 8 s SIGKILL the publisher. At T0 + 9 s start a second with `--initial-group-id 1000`, fed the leading tables, then the fixture from `first_random_access` at or after the 9 s packet, on the same timeline. Whenever the receiver exits before the track's end and before T0 + 40 s, start it again; concatenate captures and object logs. Then `check-ts.py --restart-at 8`. `recovery` passes when the first object of the second part arrives within 5 s of the second publisher's `[ready]`. YAML `recovery_ms`, number of subscriptions and how each ended.

- [ ] Throwaway run, failing tests for joining captures and the recovery check, implement, verify on both drafts, commit `git commit -m "Run the publisher restart scenario"`.

### Task 6: Image

**Files:** Create `interop/Dockerfile`, `interop/build.sh`, `.dockerignore`.

Ubuntu 24.04. Stages: moq2ts-cli (this checkout, moqxr SDK of `build.yml`'s `MOQXR_VERSION`, packages of the Linux job); moq5 at `573955c` with `scripts/setup_picoquic_deps.sh`, flags `MOQ_BUILD_ADAPTER_PICOQUIC`, `MOQ_BUILD_PQ_THREADED`, `MOQ_BUILD_SERVICE`, `MOQ_BUILD_MPEG2TS`, `MOQ_BUILD_MSF` on; the receiver (it needs `-DMOQ_PICOQUIC_SOURCE_DIR` and `-DMOQ_PICOTLS_PREFIX`); fixtures made by the runner's `make-fixture.sh` at the pinned commit, with its `test/test-check-ts.sh`; TSDuck from `tsduck_3.45-4798.ubuntu24_amd64.deb`, checksum pinned in the Dockerfile.

- [ ] Build; `docker run` with `--list` and with `RELAY_URL` against a local relay: all scenarios pass.
- [ ] Record the wall time of a full run. Over 110 s: stop and ask whether to register `ts-publisher-restart` as a second entry.
- [ ] Commit `git commit -m "Add the interop client image"`

### Task 7: WebTransport relays

The receiver has not been built or run with WebTransport. Add the moq5 WebTransport adapter to the image and run `https://` against a local relay. If it works, pass the URL through; `--done-wait` stays unavailable there (#81), so report `ts-publisher-restart` as it runs. If it does not build or run, the client reports `not ok` with that reason for `https://` and the finding goes to #81.

- [ ] Try it; record the result in the spec; commit.

### Task 8: CI publishes the image

**Files:** Modify `.github/workflows/build.yml`.

A job on `ubuntu-24.04`: `interop/build.sh` on pull requests that touch `interop/`, `src/` or the workflow; push to `ghcr.io/openmoq/moq2ts-interop-client` on tags, with `docker login` using `GITHUB_TOKEN` and `permissions: packages: write` on that job only.

- [ ] Open the PR, see the job pass, commit.

### Task 9: Runner entry

After #209 merges: repin the runner files to the runner's `main`, then add an `implementations.json` entry (client role, drafts 18 and 22, image above) in a PR to `englishm/moq-interop-runner`. Opening that PR needs the maintainer's go-ahead; draft it first.

## Open questions (on #74, unanswered)

- Is `--quic-idle-timeout 2000` acceptable for restart?
- Must the restarted capture begin at the new publisher's first access point? The scenario text says so; the checker accepts a later one.
