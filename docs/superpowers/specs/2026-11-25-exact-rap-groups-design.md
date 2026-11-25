# Groups that start at a random access point

Date: 2026-11-25
Status: approach chosen (B), design pending review
Tracking: openmoq/moq2ts#74

## Problem

Grading moq2ts with the MoQ interop runner's own MPEG-TS fixtures and
`check-ts.py` (runner PR 209) found two failures in `main`, with
`--unmodified --announce-early`:

1. A Group opens with the whole Object that holds the random access point, so
   the packets just before the point are in the new Group. This is how every
   publish from a TS source cuts Groups.
2. Under `--announce-early` the first Group keeps everything before the first
   random access point: an SDT packet, or other video. On the multi-program
   fixture the publisher also needs program 1's PMT, which follows some video,
   so a wrapper that holds all video never sees the publisher get ready.

The MSFTS draft (Section 5.2) requires only that the first Object of a Group
contain the access point. It also says a live single-program publisher SHOULD
start each Group at one. The runner requires a Group to start at one, or at PSI
immediately before it.

## Decisions

- **Exact cut on every live TS publish** (chosen over limiting it to
  `--announce-early`, which would keep two grouping behaviours). Where a random
  access point of the group PID would start a Group, the Object being filled
  ends before that packet, and the packet begins the next Object and Group.
- **Under `--announce-early`, drop the lead-in.** Nothing is published before
  the first random access point. This replaces "packets before the first random
  access point are kept". It applies to a multiplex without a reference program
  too, cut at the points of the first program.
- The two are separate pull requests: the cut first.

## Behaviour

- Live (non-seekable) sources only: FIFO, pipe, stdin, SRT, and the per-program
  pipeline. File sources and the capture path are unchanged.
- The last Object of a Group can be short, and a Group can hold one Object more
  than before. No packet is added, removed or reordered by the cut.
- PSI just before an access point stays in the previous Group. PAT and PMT
  repeat, so each Group still carries them (the draft's SHOULD).
- The cut applies only where a new Group would start. The first access point of
  a track that kept its lead-in starts no Group, as before.
- A packet carried over for the cut is not a source gap and not a timed start.
- A flagged packet that continues a flagged access unit (no
  `payload_unit_start_indicator`) is no new access point: ISO/IEC 13818-1 lets
  the flag repeat.
- Lead-in drop: while discarding, a quiet input still answers "nothing yet", so
  the publisher serves the relay. Discarding stops, with a warning, after 10 s
  of PCR time (or 20,000 packets without a PCR) with no random access point, as
  the look-ahead of a normal live start does.

## Evidence

With throwaway versions of both changes, the runner's carriage, clock step and
multi-program fixtures passed every check on drafts 22 and 18, three rounds
each, through a loopback `moq-relay`; the catalog check is skipped. The
prototype cut only under `--announce-early` and had no bound on the lead-in
drop.

## Not covered

SRT ingest, M2TS (192-byte) input and per-program (`--config`) publishing have
not been run with the cut. The plan tests them in unit tests; SRT needs a manual
run before a release.
