# Interop client: publisher and receiver changes Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Give `moq2ts-cli` and `moq2ts-receiver` what an MPEG-TS interop client needs: a subscriber that subscribes before media starts gets the whole stream, and both tools report what the runner's checks need.

**Architecture:** The pipeline's object source gains a "nothing yet" answer so moqxr keeps serving control messages while the input is quiet. An opt-in early catalog lets the publisher announce from the PAT and PMT alone. The receiver and publisher gain the log outputs the wrapper reads.

**Tech Stack:** C++20, Qt 6, moqxr v0.4.25 SDK, moq5 (receiver, C11), CMake/CTest.

**Spec:** `docs/superpowers/specs/2026-11-25-interop-client-design.md`

The wrapper, the image and CI publishing are a second plan, written after this one lands.

## Global Constraints

- Early catalog and all new log output are opt-in. A normal live publish behaves as before, apart from answering control messages while its input is quiet.
- The early catalog omits `mpeg2tsRandomAccess`.
- Drafts 22 and 18 only for the receiver.
- Build against the moqxr release pinned in `.github/workflows/build.yml` (`-DMOQ2TS_BUILD_WITH_MOCK_MOQXR=OFF -DMOQXR_SDK_DIR=...`).
- No emoji in commit messages; no co-author or generator lines.
- Work on a branch from `main`.

## Review Focus

- Input ends while the source is idle: the broadcast must still end with the closing catalog, once. (Task 2)
- SIGINT while the input is quiet: the publisher must exit promptly. (Task 2)
- Input stalls mid-Group: the partial Object still goes out, then the source reports idle. (Task 2)
- `--announce-early` on a multiplex whose first PMT is late: open fails or waits as today, never declares a track without its tables. (Task 3)
- `--announce-early` input that begins mid-GOP: the first Group is allowed to start without a random access point, and the catalog must not claim one. (Task 3)

---

### Task 1: A source can answer "nothing yet"

**Files:**
- Modify: `src/publish/MoqxrPublisher.h` (`PublishedObject`), `src/publish/ObjectLookahead.h`, `src/publish/MoqxrPublisher.cpp` (live source lambda and `publishLiveObjectsMock`), `src/publish/RedundantPublisher.cpp`
- Test: `tests/object_lookahead_test.cpp`, `tests/redundant_publisher_test.cpp`

**Interfaces:**
- Produces: `PublishedObject::idle` (`bool`, default false). A source returning an idle object means "no Object yet"; `std::nullopt` still means end of source.

- [ ] **Step 1: Write the failing tests**
  - `object_lookahead_test`: source yields media group 0 object 0, then idle, then media group 1 object 0, then end. Assert `next()` returns the idle object first with the media object still held (`pending() == 1`), then group 0 with `finalInGroup == true`, then group 1, then `nullopt`.
  - `redundant_publisher_test`: with a `TestPath`, a producer yielding idle then one object: the path receives no queued idle object from the producer, and its source returns an idle object while its queue is empty and open.
- [ ] **Step 2: Run them and see them fail**
  Run: `cmake --build build --parallel && ctest --test-dir build -R 'object-lookahead|redundant-publisher' --output-on-failure`
  Expected: FAIL (compile error: no member `idle`).
- [ ] **Step 3: Implement**
  - `ObjectLookahead::next()` returns an idle object at once, releasing nothing and leaving held objects held.
  - `MoqxrPublisher` live lambda returns `std::nullopt` for an idle object, and sets `source.is_finished` to return `state->sourceEnded`. Check that every existing `nullopt` return that ends the publish also sets `sourceEnded` or stops through `stop_requested_`.
  - `publishLiveObjectsMock` skips idle objects.
  - `RedundantPublisher`: the producer does not push idle objects; each path's source returns an idle object when `pop(kPopWait)` times out on an open queue.
- [ ] **Step 4: Run the tests and see them pass**
  Run: `ctest --test-dir build --output-on-failure`
  Expected: all pass.
- [ ] **Step 5: Commit** `git commit -m "Let a live source say it has no Object yet"`

### Task 2: The pipeline reports a quiet input

**Files:**
- Modify: `src/media/M2tsPacketizer.h`, `src/media/M2tsPacketizer.cpp`, `src/media/LivePipeline.cpp` (the file/stdin `nextObject`)
- Create: `tests/live_pipeline_idle_test.cpp`
- Modify: `CMakeLists.txt` (add `moq2ts-live-pipeline-idle-tests` beside `moq2ts-live-pipeline-end-tests`, `TIMEOUT 60`)

**Interfaces:**
- Consumes: `PublishedObject::idle`.
- Produces: `bool M2tsPacketizer::inputQuiet(int msecs)`: true only for a sequential source with no buffered packet (`m_carryPacket` empty, `m_prebufferPos >= m_prebuffer.size()`, fewer than `m_packetSize` bytes left in `m_unread`) and no packet readable within `msecs`. Always false on Windows, which keeps today's blocking read.

- [ ] **Step 1: Write the failing test**
  A FIFO and a recording `IMoqOutput` that calls the source and logs each object. Write PAT, PMT and one random access point, keep the write end open, wait 500 ms. Assert:
  - the media Object holding those packets was delivered;
  - at least one idle object followed it;
  - after writing more packets, further media Objects arrive with increasing Object IDs;
  - after closing the write end, exactly one object has `endsBroadcast`, then `nullopt`;
  - in a second case, clearing `running` while idle makes the source return `nullopt` within 1 s.
- [ ] **Step 2: Run it and see it fail**
  Run: `ctest --test-dir build -R live-pipeline-idle --output-on-failure`
  Expected: FAIL, no idle object (the source blocks).
- [ ] **Step 3: Implement** `inputQuiet` with the existing `packetBefore` helper; in the `nextObject` lambda, after the `st->pending` branch, return an idle object when `inputQuiet(20)`.
- [ ] **Step 4: Run all tests and see them pass**
  Run: `ctest --test-dir build --output-on-failure`
- [ ] **Step 5: Commit** `git commit -m "Keep serving the relay while a live input is quiet"`

The capture path and `MptsPipeline` are unchanged.

### Task 3: Early catalog (`--announce-early`)

**Files:**
- Modify: `src/app/PublishConfig.h`, `src/media/M2tsPacketizer.h`, `src/media/M2tsPacketizer.cpp`, `src/media/LivePipeline.cpp`, `src/cli/main_cli.cpp`, `docs/CLI.md`
- Test: `tests/m2ts_packetizer_test.cpp`
- Create: `tests/early-announce-e2e.sh`

**Interfaces:**
- Consumes: Tasks 1 and 2.
- Produces: `PublishConfig::announceEarly` (`bool`), `void M2tsPacketizer::setAnnounceEarly(bool)`, CLI flag `--announce-early`.

- [ ] **Step 1: Write the failing tests** (FIFO, write end held open)
  - Writing only SDT, PAT, PMT (three 188-byte packets): `open()` returns true within 2 s; `randomAccess()` is false; `measuredMuxRate()` is 0.
  - The same with only PAT and PMT (two packets).
  - The catalog built from it has no `mpeg2tsRandomAccess` and has `initRef`.
  - A two-program PAT where the first program's PMT is not yet written: `open()` has not returned after 500 ms.
  - Packets after the tables that start mid-GOP are all in Group 0 (none dropped).
- [ ] **Step 2: Run and see them fail**
  Run: `ctest --test-dir build -R m2ts-packetizer --output-on-failure`
- [ ] **Step 3: Implement.** With the flag set, `open()` peeks `188 * 2` bytes for size detection instead of `192 * 4`, skips `measureRates`, sets `m_dropLeadIn` false and does not call `findRandomAccess()`. The CLI refuses `--announce-early` without `--unmodified`, with `--config`, or with an SRT input, with an error naming the flag (exit code as other option errors).
- [ ] **Step 4: Run all tests and see them pass**
- [ ] **Step 5: Write `tests/early-announce-e2e.sh`**, following `tests/srt-mpts-e2e.sh` for relay start-up (`MOQ5_RELAY` or `RELAY_URL`, `MOQ2TS_CLI`, `MOQ2TS_RECEIVER`): generate a 20 s single-program TS with ffmpeg (constant mux rate, 1 s closed GOP), feed the packets before the first video packet, wait for `[ready]`, start the receiver, wait for its `[rx] subscription active` line (Task 5; until then wait for `[rx] draft=`), pace the rest at the mux rate, close. Pass when the capture's non-null packets equal the source's, for `DRAFT=22` and `DRAFT=18`.
- [ ] **Step 6: Run it** and record the result. Expected: both drafts pass. The prototype did on loopback `moq-relay`.
- [ ] **Step 7: Document** `--announce-early` in `docs/CLI.md`, including that the catalog does not declare random access.
- [ ] **Step 8: Commit** `git commit -m "Add --announce-early to publish the catalog before media"`

### Task 4: Group log (`--log-groups`)

**Files:**
- Modify: `src/publish/MoqxrPublisher.h`, `src/publish/MoqxrPublisher.cpp`, `src/publish/RedundantPublisher.h`, `src/publish/RedundantPublisher.cpp`, `src/cli/main_cli.cpp`, `docs/CLI.md`
- Test: `tests/live_pipeline_idle_test.cpp` (mock publisher)

**Interfaces:**
- Produces: signal `MoqxrPublisher::groupOpened(const QString& trackName, qulonglong groupId)`, emitted with the wire Group ID when the first Object of a new Group of a media track is handed to the transport (mock path too); CLI flag `--log-groups` printing `[group] track=<name> id=<n>` to stdout, flushed.

- [ ] **Step 1: Write the failing test:** publish a source with two Groups through the mock transport; the signal fires exactly for ids 0 and 1 of the media track and never for `catalog` or the timeline track.
- [ ] **Step 2: Run and see it fail.**
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run all tests and see them pass.**
- [ ] **Step 5: Commit** `git commit -m "Add --log-groups"`

### Task 5: Receiver outputs for the checks

**Files:**
- Modify: `src/receive/main.c`, `tests/receiver-cli-smoke.sh`, `tests/early-announce-e2e.sh`, `docs/CLI.md`

**Interfaces:**
- Produces: `--object-log FILE`, one line `<group> <object> <bytes>` per Object written to `--out`, in write order; `--subscribe-after SECONDS` (0 to 3600, decimals allowed), counted from process start; stderr line `[rx] subscription active`, once, when `moq_media_receiver_track_state` first reports `MOQ_MEDIA_TRACK_STATE_ACTIVE`; stderr line `[rx] subscribe sent` when `moq_media_receiver_subscribe_track` is called.

- [ ] **Step 1: Write the failing smoke checks:** `--help` lists both options; `--subscribe-after` with `-1`, `abc`, empty or `3601` exits 2; `--subscribe-after 8.5` is not a usage error; an `--object-log` path in a missing directory exits 1 before connecting.
- [ ] **Step 2: Run and see them fail**
  Run: `ctest --test-dir build-receiver --output-on-failure`
- [ ] **Step 3: Implement.** Flush the object log with the capture; a failed write is fatal, as for `--out`.
- [ ] **Step 4: Run and see them pass.**
- [ ] **Step 5: Extend the e2e script:** assert the log's byte column sums to the capture size, Group IDs never decrease, and `[rx] subscription active` appears before media is paced. Run it for both drafts.
- [ ] **Step 6: Commit** `git commit -m "Add receiver outputs for stream checks"`

## Left for the second plan

The wrapper and TAP output, the five scenarios, the image, the CI publish job, the runner entry, and checking against the runner's own fixtures and `check-ts.py` (including its `group-starts` rule on a first Group that begins with SDT, PAT and PMT, and a multiplex whose tables are spread out).
