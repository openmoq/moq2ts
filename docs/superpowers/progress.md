# Progress: MoQ interop client and related work

Last updated: 2026-11-26. Update this file when work stops.

## Resume here

1. `git switch feat/interop-client`: pushed, with its pull request open
   against `main` (rebased after #85).
2. Plan `docs/superpowers/plans/2026-11-25-interop-client-image.md`: Tasks 1 to
   7 done. Task 8 (CI builds and publishes the image) is committed but has not
   run on GitHub. The whole-branch review is done and its findings fixed;
   deferred minor points are in the ledger.
3. Next: wait for CI on the PR (its image job completes Task 8); merge only on
   the maintainer's word. After the first `v*` tag the ghcr package has to be
   made public by hand.
4. Task 9 (runner entry) waits for moq-interop-runner#209; ask first.
5. The tail loss is fixed by #85 (`--close-delay`); the rebased image passed
   45 of 45 full runs.
6. Not filed: with an `https://` URL that has no path, the publisher uses `/`
   and the receiver `/moq`. The client works around it.

## Waiting on others

| Item | What we asked | Effect |
| --- | --- | --- |
| [#60](https://github.com/openmoq/moq2ts/issues/60), gmarzot | Rerun the 10-minute transparent run on `v1.4.0-rc4`; logs for the 22,571 out-of-order refusals | Hold final 1.4.0 until it comes back |
| [#74](https://github.com/openmoq/moq2ts/issues/74), t0ms | Is `--quic-idle-timeout 2000` acceptable; must a restarted capture start at the first access point | Restart scenario (plan Task 5) |
| [moq-interop-runner#209](https://github.com/englishm/moq-interop-runner/pull/209) | Open, unreviewed at `d2fbb02` | Runner entry (Task 9) |
| [moq5#75](https://github.com/openmoq/moq5/pull/75) | Open, conflicts with moq5 main | WebTransport terminal wait, [#81](https://github.com/openmoq/moq2ts/issues/81) |

## Done (2026-11-25)

- `main` at `4c9e653`. `v1.4.0-rc4` published as a prerelease; v1.3.0 is Latest.
  The release job does not set the prerelease flag: run
  `gh release edit <tag> --prerelease` after it finishes.
- #79: AppImages built on Ubuntu 24.04 (pinned), `--version` from CMake, 1.4.0.
- #78 (gwendalsimon): sent counts kept across reconnects.
- #80, closes #73: receiver `--done-wait`, 2 s default on `moqt://`. Restart gap 41 s to 13 s.
- #82: live source can answer "nothing yet"; `--announce-early`, `--log-groups`,
  receiver `--object-log`, `--subscribe-after`, subscription state lines.
- #83: every live TS Group starts at its random access point.
- #84: `--announce-early` publishes nothing before the first access point.
- Runner fixtures graded with `check-ts.py` through a loopback `moq-relay`:
  carriage, clock step and multi-program pass every check (catalog skipped) on
  drafts 22 and 18. This was a local script, not the client. Late subscriber and
  restart have not been run against the checker.

## Other open items (not part of the next plan)

- [#81](https://github.com/openmoq/moq2ts/issues/81): WebTransport still waits 30 s.
- [#77](https://github.com/openmoq/moq2ts/issues/77): `--config` programme join time.
- [#68](https://github.com/openmoq/moq2ts/issues/68), [#69](https://github.com/openmoq/moq2ts/issues/69), PR #61 (gwendalsimon): wait for the next MSFTS draft; #61 has conflicts.
- Not covered by any run: Windows; SRT with the quiet-input change; M2TS with early announce.
- `moq2ts-srt-listener-tests` and `moq2ts-srt-link-stats-tests` fail intermittently
  (about 1 run in 8 alone). Not investigated; not filed.

## Deferred review findings

- #82: the last Object before an input stall is held until input resumes; a PAT
  change just as input goes quiet waits for more input; `--announce-early` is
  accepted for a regular file; `[group]` memory not cleared across publishes;
  receiver may miss a brief active state, accepts hex for `--subscribe-after`,
  leaves the object log unclosed on some error exits.
- #83: the first timed Group can hold a first access point mid-Object; a PMT with
  no video and no PCR PID can start a Group mid-Object; missing tests for the gap
  warning, quiet-after-cut, and M2TS with gap or per-program.
- #84: the pipeline idle test would pass without the empty-payload branch; the
  bound warning says "10 s" for the packet bound too; no log while waiting at low
  rate without PCR; a source that ends before any access point exits 0 silently.

## Rebuilding the local test tools

The tools used today lived in a session scratchpad, which does not survive. To
recreate them:

- Publisher: `cmake -DMOQ2TS_BUILD_WITH_MOCK_MOQXR=OFF -DMOQXR_SDK_DIR=<sdk>`, the
  SDK from `gh release download v0.4.25 --repo openmoq/moqxr --pattern '*Linux*'`.
- moq5 at `573955c` (or later main): `scripts/setup_picoquic_deps.sh`, then CMake
  with `MOQ_BUILD_ADAPTER_PICOQUIC`, `PQ_THREADED`, `SERVICE`, `MPEG2TS`, `MSF` on,
  and install. Receiver: `cmake -S src/receive` with `CMAKE_PREFIX_PATH` and the
  two `MOQ_PICOQUIC_SOURCE_DIR` / `MOQ_PICOTLS_PREFIX` paths.
- Relay: moq-dev `b45179d39b`, `cargo build --release -p moq-relay`; run with
  `--listen 127.0.0.1:PORT --listen-tls-generate localhost --auth-public ''`.
  The `../moq` checkout's own relay (0.17.1) rejects draft-22 receivers.
- Checker: a `debian:trixie-slim` image with ffmpeg, python3 and TSDuck
  3.45-4798, as the runner's `builds/moq-net-media-ts/Dockerfile.client`; run the
  runner scripts with `--network none`.
- `ctest` covers the publisher; the receiver has two tests in its own build; CI
  does not build the receiver. `tests/early-announce-e2e.sh` needs a relay
  (`INSECURE=1` with a self-signed one).
- Grading recipe: feed `moq2ts-cli --announce-early --unmodified --log-groups`
  the packets before the first video packet with `random_access_indicator`, wait
  for `[ready]`, start the receiver with `--object-log`, wait for
  `[rx] subscription active`, pace the rest at `size / 20 s`, close stdin, then run
  `check-ts.py --fixture F --received R --objects O`.

## How this work has been run

Design in `docs/superpowers/specs/`, plan in `docs/superpowers/plans/`, one
branch and PR per change, an independent review before each PR, merge only on the
maintainer's word. Ask before posting on GitHub, tagging or releasing.
