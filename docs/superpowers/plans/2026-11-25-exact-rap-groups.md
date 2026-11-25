# Groups that start at a random access point Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Every live TS publish starts each Group at its random access point, and `--announce-early` publishes nothing before the first one.

**Architecture:** `M2tsPacketizer::readObject` ends the Object before a packet that will start a Group and carries that packet over, as it already does for timed starts. The early-announce lead-in is dropped lazily in the same loop, which returns an empty Object while the input is quiet.

**Tech Stack:** C++20, Qt 6, moqxr v0.4.25 SDK, CMake/CTest.

**Spec:** `docs/superpowers/specs/2026-11-25-exact-rap-groups-design.md`

## Global Constraints

- Live (non-seekable) sources only. File sources and the capture path behave as before.
- The cut adds, removes and reorders no packet.
- Lead-in drop only under `--announce-early`; its bound is 10 s of PCR time, or 20,000 packets without a PCR (`kRapWindowPcr`; `kTimedWindowMaxPackets` with timed windows).
- Build against the pinned moqxr SDK (`-DMOQ2TS_BUILD_WITH_MOCK_MOQXR=OFF -DMOQXR_SDK_DIR=...`).
- No emoji in commit messages; no co-author or generator lines.
- Two pull requests: Task 1 on `feat/exact-rap-groups`; Task 2 on a branch from `main` after Task 1 merges.

## Review Focus

- The source ends, or goes quiet, right after a packet is carried over: it must still be published, once. (Task 1)
- A PAT or PMT change on the packet where a cut falls: the track end or catalog update behaves as before. (Task 1)
- SRT source gap and access point at the same packet: one Group starts, reported as a gap. (Task 1)
- 192-byte M2TS input: the timestamp prefix travels with the carried packet. (Task 1)
- A source with no `random_access_indicator` at all under `--announce-early`: it publishes after the bound instead of staying silent. (Task 2)

---

### Task 1: End the Object before the access point

**Files:**
- Modify: `src/media/M2tsPacketizer.h`, `src/media/M2tsPacketizer.cpp` (`readObject`)
- Test: `tests/m2ts_packetizer_test.cpp`
- Modify: `docs/CLI.md` or `README.md`, wherever Group boundaries of a live source are described

**Interfaces:**
- Produces: for a sequential source, an Object with `startsGroup` set by a random access point has that packet first. `m_carryTimed` is renamed `m_carryIsBoundary` (a carried packet that is not a source gap).

- [ ] **Step 1: Write the failing tests** (a `SequentialDevice` stream: PAT, PMT, then three access points on PID 0x100, each followed by a few video packets, with a count between them that is not a multiple of `packetsPerObject = 4`)
  - every Object with `startsGroup` begins with a packet that has `random_access_indicator` on PID 0x100;
  - the Group ID rises by one at each access point and Object IDs restart at 0;
  - the concatenated payloads equal the input from the first access point;
  - the same for the 192-byte form, comparing source packets with their prefixes;
  - the stream ending on the access point packet itself publishes it once, as the first Object of its Group;
  - a source gap recorded at the offset of an access point starts one Group, not two;
  - per-program mode (`setTransparent(false)`) starts each Group at the access point;
  - a seekable file source produces the same Object sizes as before.
- [ ] **Step 2: Run and see them fail**
  Run: `cmake --build build --parallel && ctest --test-dir build -R m2ts-packetizer --output-on-failure`
- [ ] **Step 3: Implement.** In the read loop, after the sync check and PID extraction and before the PCR and PSI handling: when `m_sequential`, `payload` is not empty, `m_sawFirstRap || m_timedGroups > 0`, `pid == m_rapPid` and `hasRandomAccessIndicator(tsView)`, move the packet to `m_carryPacket` with its offset, set the boundary flag, clear carried gap causes, and break. Test the indicator without `startsRandomAccess()`, which latches a PID.
- [ ] **Step 4: Run all tests and the source guards; see them pass**
  Run: `ctest --test-dir build --output-on-failure` and `for f in $(grep -L moqxr tests/*-guards.sh); do bash $f; done`
  An existing test that pins the old Object sizes is updated only if its new expectation follows from the spec; record each in the ledger.
- [ ] **Step 5: Run the end-to-end check** `tests/early-announce-e2e.sh` on drafts 22 and 18. Expected: pass.
- [ ] **Step 6: Document** that a live source's Group starts at the access point.
- [ ] **Step 7: Commit** `git commit -m "Start each live Group at its random access point"`

### Task 2: Drop the lead-in under --announce-early

**Files:**
- Modify: `src/media/M2tsPacketizer.h`, `src/media/M2tsPacketizer.cpp`, `src/media/LivePipeline.cpp`, `docs/CLI.md`
- Test: `tests/m2ts_packetizer_test.cpp`, `tests/live_pipeline_idle_test.cpp`
- Modify: `tests/early-announce-e2e.sh`

**Interfaces:**
- Consumes: Task 1.
- Produces: `readObject` may return true with an empty `object->payload`, meaning nothing to publish yet; only while discarding the early lead-in on a quiet input. `LivePipeline` turns it into an idle Object.

- [ ] **Step 1: Write the failing tests**
  - Replace "early: no packet is dropped" and "stay in Group 0": the first Object of Group 0 starts with the first access point, and nothing before it is published.
  - A two-program multiplex without a reference program drops the lead-in too, cut at the first program's access points.
  - FIFO with the write end open, fed tables and non-access-point video: `readObject` returns with an empty payload within 1 s.
  - Through the pipeline, the same input makes the source report idle.
  - 11 s of PCR time with no access point: packets are published from then on, with one warning.
  - Without `--announce-early`, a multiplex without a reference program still keeps its lead-in.
- [ ] **Step 2: Run and see them fail**
- [ ] **Step 3: Implement.** `m_dropLeadIn = m_sequential && (m_announceEarly || <today's condition>)`. In the drop loop, when early and `payload` is empty and `inputQuiet(20)`, return true with the empty payload; count discarded PCR time with a `PcrSpan` and discarded packets, and stop dropping at the bound.
- [ ] **Step 4: Change the e2e script** to feed the packets before the first access point, and to expect the source's non-null packets from that point. Run drafts 22 and 18.
- [ ] **Step 5: Run all tests and guards; see them pass**
- [ ] **Step 6: Update** the `--announce-early` row in `docs/CLI.md`.
- [ ] **Step 7: Commit** `git commit -m "Publish nothing before the first access point with --announce-early"`

### Check against the runner (after Task 2)

Generate the fixtures with the runner's `make-fixture.sh` at the pinned commit, publish each through a relay, and run `check-ts.py` with `--objects`. Expected: carriage, clock step (`--expect-step`) and multi-program (`--programs 3`) pass every check but the skipped catalog, on drafts 22 and 18. Run the third-party scripts in a container without network.
