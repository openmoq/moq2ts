# moq2ts client for the MoQ interop runner: MPEG-TS scenarios

Date: 2026-11-25
Status: approved
Tracking: openmoq/moq2ts#74

## Intent

t0ms asked that moq2ts be registered as a client for the runner's MPEG-TS
scenarios, so that the runner grades moq2ts against each relay it tests, with
the same fixtures and checker as the moq-dev client. The maintainers agreed.

The scenarios are `ts-carriage-integrity`, `ts-late-subscriber`,
`ts-clock-step`, `ts-multi-program` and `ts-publisher-restart`. The last three
are in englishm/moq-interop-runner#209, which is not merged yet.

## Goals

- A container image that runs the five scenarios against `RELAY_URL` and
  reports TAP version 14, as the runner's `docs/TEST-CLIENT-INTERFACE.md`
  describes.
- Verdicts from the runner's `check-ts.py`.
- The image builds from this repository, and CI publishes it.

## Non-goals and anti-goals

- No per-program tracks: the scenarios carry the whole multiplex.
- No MOQT drafts other than 22 and 18, which are what `moq2ts-receiver` takes.
- No change to the runner's checks or fixtures to make moq2ts pass.
- A check that cannot be run is reported as skipped, never as passed.

## Approaches

| Approach | Trade-off |
| --- | --- |
| A. Image in moq2ts, published by its CI (chosen) | Builds and tests with our code; the runner change is an `implementations.json` entry. We keep a Dockerfile and a publish job. |
| B. Image in the runner under `builds/moq2ts/` | Matches the moq-dev client, but each change of ours needs a runner change and our CI does not test it. |

A was chosen. It also matches the request on #74 that the image be part of
moq2ts's CI.

## Design

**Image.** Ubuntu 24.04 base. Holds `moq2ts-cli` built on the pinned moqxr SDK
release, `moq2ts-receiver` built on a pinned moq5 commit that has
`done_wait_timeout_us` (openmoq/moq5#74), ffmpeg, TSDuck, Python, and the
runner's fixtures and `check-ts.py` taken from a pinned runner commit.

**Wrapper.** A Python script (the image needs Python for the checker). It reads
`RELAY_URL`, `TESTCASE`, `TLS_DISABLE_VERIFY` and `VERBOSE`, paces the fixture
to its PCR, runs publisher and receiver, calls `check-ts.py`, and prints TAP.
Exit codes: 0 pass, 1 failure, 127 unsupported test. It names the packaging in
`ts_packaging`.

**Receiver options.**

- `--object-log FILE`: `<group> <object> <bytes>` per object written, in arrival
  order.
- `--subscribe-after SECONDS`, and a log line when the track subscription
  becomes active (the track state leaves PENDING).

**Publisher option.** A log line for each Group opened, so that the wrapper can
report the join-point groups.

**Restart scenario.** The wrapper kills the publisher with SIGKILL at T0 + 8 s,
starts a new one at T0 + 9 s, restarts the receiver whenever its track ends,
joins the captures and times the recovery. The publisher runs with
`--quic-idle-timeout 2000`; without it, recovery measured 13.4 s against the
5 s limit.

**Catalog check.** Skipped, and reported as skipped, in the first version:
moq5's media receiver was not found to expose the raw catalog.

**Time budget.** The runner gives one client run 120 s. The five scenarios take
112 to 114 s one after another on a draft-22 relay, and more where draft 22 is
tried first and refused, so the client runs them at once, each in its own
namespace (27 s). The cost: five streams load the relay together, so a relay
could fail a scenario it passes alone. `MEDIA_SEQUENTIAL=1` runs them one at a
time. Chosen over registering two entries.

## Risks, to be tested first

1. **Start of the stream.** The scenarios subscribe before media starts, and the
   publisher sends no catalog until it has read the PAT and PMT from its input.
   The first plan was to feed the fixture's leading tables, wait for the
   receiver to subscribe, then pace the rest. It does not work; see below.
2. **WebTransport.** The receiver has not been built or run with it. Restart
   over WebTransport also depends on #81.
3. **TSDuck on Ubuntu 24.04.** Package availability at the runner's version is
   unchecked.

### Result for risk 1 (2026-11-25): the plan above does not work

Throwaway test over loopback through `moq-relay`, draft 22, with a 20 s fixture
generated to the shape the runner describes.

- Fed only the leading tables, the publisher did not connect to the relay until
  about a second after media started. A receiver started before that found no
  catalog and exited.
- A receiver started as soon as the publisher printed `[ready]` (0 s and 0.5 s
  after, four runs) began at Group 2, missing the first part of the fixture.
  Starting it 2 s later, it began at Group 4. Each capture ran unbroken to the
  end of the fixture.

So the carriage scenarios need a change that lets the subscriber get the stream
from its first random access point.

### Start-up approach (chosen: A)

| Approach | Trade-off |
| --- | --- |
| A. The publisher announces before media, and the wrapper holds the media until the subscription is active (chosen) | Follows the scenario's own order and should not depend on relay caching. Changes publisher start-up. |
| B. The receiver asks the relay for the Groups from the start | Leaves the publisher alone, but depends on each relay and draft supporting it and still holding them. |

A needs three parts. All are opt-in or invisible to a normal live publish.

1. **Early catalog** (`moq2ts-cli --announce-early`). The publisher builds the
   catalog from the PAT and PMT alone, without the look-ahead for random access
   points. The catalog then omits `mpeg2tsRandomAccess`, which the draft makes
   optional: absent means no guarantee. Groups still start at random access
   points as they are read.
2. **A source that does not block.** moqxr handles control messages and pulls
   Objects in one loop. Our reader blocks until input arrives, so a publisher
   waiting for media would not answer the SUBSCRIBE the wrapper is waiting for.
   The publisher must tell moqxr "nothing yet" when the input is quiet, using
   `LiveObjectSource::is_finished`, which moq2ts does not set today. This also
   touches the interface between the pipelines and the publisher, including the
   multi-relay path.
3. **Receiver signal.** The log line for an active subscription, already in the
   design, tells the wrapper to start the media.

The restart scenario's second publisher does not hold its media: the receiver
resubscribes only after the relay ends the old subscription.

Read from the moqxr v0.4.25 source, not run: with no subscriber yet, a live
publish reads its source and drops the Objects. Holding the input is what
prevents that.

**To prove first**, with a throwaway change: that a publisher changed this way
answers the subscription while its input is quiet, and that the capture then
starts at the fixture's first random access point. If it does not, return to
this section before planning further.

### Result for approach A (2026-11-25): it works

Throwaway change in a scratch clone, loopback `moq-relay`, the same 20 s fixture.
Only the three leading tables were fed, then the receiver was started, and the
media was paced once the subscription was active. The publisher was ready 0.2 s
after receiving the tables. On draft 22 (four runs) and draft 18 (two runs) the
capture held every non-null packet of the fixture, first to last.

Two things the implementation must get right, found while doing it:

- Packet-size detection waits for four packets; the fixture's leading tables are
  three. With the early catalog it must wait for two.
- "The input is quiet" must count the packetizer's own buffered bytes, which are
  consumed by position and held in two buffers.

Not tried: a multiplex, M2TS (192-byte) input, multiple relays, the runner's own
fixtures and checker.

## Open questions on #74

- Is `--quic-idle-timeout 2000` acceptable for `ts-publisher-restart`?
- Must the restarted capture begin at the new publisher's first random access
  point? The scenario text says so; `check-ts.py` accepts a later one.

## Order of work

1. A throwaway test of start-up approach A.
2. Carriage integrity, clock step and multi-program.
3. Publisher restart.
4. Late subscriber.
5. CI publishing, then the runner change once #209 merges.
