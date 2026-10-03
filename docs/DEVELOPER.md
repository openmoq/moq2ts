# Developer Guide — MOQ2TS Publisher

## Purpose

`moq2ts-publisher` is a reference cross-platform Qt application for live
media publishing. It currently provides:

1. Source selection (TS/M2TS video/audio)
2. Direct TS/M2TS source packet validation and objectization
3. Catalog generation for `draft-gregoire-moq-msfts`
4. Program-level packet filtering for selected MPEG-TS programs
5. Camera and microphone enumeration through libavdevice/platform capture APIs
6. In-process camera/microphone capture through libavdevice/libavformat
7. Preview tab for selected camera/microphone video and audio level verification
8. MSF timeline side-track publication for wall-clock correlation
9. Publication through an adapter that defaults to a deterministic mock path

## Runtime flow

- The UI (`src/app/MainWindow.*`) builds a `PublishConfig` from operator input and
  emits `startRequested`.
- The UI enumerates camera and microphone devices through libavdevice/platform
  capture APIs, with Qt Multimedia fallback where enabled. These selections are
  stored in `PublishConfig`.
- The Preview tab can start a preflight libav preview worker for selected
  camera/microphone devices without connecting to a relay.
- Main wires this into `LivePipeline` and `MoqxrPublisher`.
- `LivePipeline` opens one TS/M2TS source and passes the selected program number
  to the packetizer. Program `0` means the first nonzero PAT program.
- If no TS/M2TS file source is provided and a camera or microphone is selected,
  `LivePipeline` opens `LibavCaptureSource` instead.
- While live capture publishing is running, `LibavCaptureSource` emits preview
  video frames and audio RMS levels from the same decoded frames that feed the
  encoder. The UI displays those levels on a dBFS meter scale.
- `M2tsPacketizer` detects 188-byte TS or 192-byte M2TS source packets and
  validates sync bytes.
- `LibavCaptureSource` opens selected devices through libavdevice, transcodes
  to H.264 plus AAC/Opus, muxes MPEG-TS into an in-memory AVIO sink, extracts
  PAT/PMT as `initData`, and emits 188-byte TS packet objects.
- On Linux/V4L2, `LibavCaptureSource` probes the selected camera's device nodes
  (`V4l2Capabilities`) and prefers an MJPEG mode that meets the requested
  width/height/fps, opening it with `input_format=mjpeg`; it falls back to raw
  once if the MJPEG open fails. The encoder rate and GOP are reconciled to the
  framerate the driver actually negotiates, so timestamps and IDR spacing match
  reality even when a camera cannot honor the requested rate. See the
  "Camera capture (V4L2)" section below.
- The decode loop tolerates a corrupt input frame (e.g. transient MJPEG garbage
  some cameras emit at startup): an `AVERROR_INVALIDDATA`/`EINVAL` from
  `avcodec_send_packet`/`avcodec_receive_frame` skips that packet and continues,
  mirroring ffmpeg; other decode errors still abort the session.
- Video frames are scaled to the encoder's `YUV420P` with the swscale color
  range set explicitly (MJPEG sources are full-range `yuvj*`; the encode target
  is limited-range), and the same `applyScalerRange` handling drives the
  full-range BGRA preview conversion.
- Encoded video keyframes (`AV_PKT_FLAG_KEY`) mark MOQT group boundaries: the
  muxer interleaver is flushed at each IDR so the byte offset is the exact start
  of the keyframe's TS packets, making each MOQT group start on a random-access
  point. The GOP is bounded by `keyframeIntervalMs` so IDRs recur periodically.
- `M2tsPacketizer` scans PAT/PMT, selects one program, and filters media
  objects to PAT, selected PMT, selected PCR PID, and that program's elementary
  PIDs. Packets from other programs and null packets are not published.
- `MsftsMuxer` builds the MSF catalog: an `mpeg2ts` media track plus an
  optional media-timeline side-track. The media track carries MSF common
  fields (`isLive`, `role`, `mimeType`, `targetLatency` when live, optional
  `bitrate`) and MSFTS mpeg2ts fields (`mpeg2tsMode`, `mpeg2tsPacketSize`,
  `mpeg2tsProgramNumber` and optional `mpeg2tsPcrPid` on the `per-program`
  and `unmodified-program` modes, and `mpeg2tsRandomAccess` when the first
  Object of every Group contains a random access point).
- `MsftsMuxer` also adds a `<stream>.timeline` track of MSF
  `packaging: "mediatimeline"` that `depends` on the media track.
- Whole source packets are grouped into MOQT Object payloads and exposed to
  `MoqxrPublisher::publishLiveObjects(...)`.
- `LivePipeline` adds one timeline record per media Group and publishes the
  whole record history as the first Object of the timeline Group of the same
  number (MSF Section 7.3).

## File-level map

- `src/app/MainWindow.h/.cpp`
  - Qt UI and user controls.
  - Emits start/stop events and receives status/log callbacks.
  - Owns the Config, Preview, and Logs tabs.

- `src/app/PreviewPanel.h/.cpp`
  - Displays decoded preview video frames.
  - Displays left/right audio meters using dBFS scaling.
  - Starts a preflight `LibavPreviewWorker` for selected devices and receives
    live publishing preview callbacks from `LivePipeline`.

- `src/media/LivePipeline.h/.cpp`
  - Packetizer setup, catalog publication, media object loop, and pacing.
  - Uses `std::thread`.

- `src/media/M2tsPacketizer.*`
  - Detects TS/M2TS packet size.
  - Emits object payloads made only from whole source packets.
  - Scans the initial packet window for PAT and PMT packets, preserves those
    packets in source-packet form, and exposes them for catalog `initData`.
  - Filters MPTS inputs down to the selected program's PSI/PCR/elementary PIDs
    before objectization, with a PAT rewritten to list that program only.
  - Keeps the conditional access packets of the selected program: the ECM
    PIDs of its PMT CA_descriptors, the CAT rewritten to the CA systems those
    ECMs use, and the EMM PIDs that the CAT gives for them.
  - With `--retain-si`, reduces the SDT actual to the carried service and
    keeps only that service's EIT actual sections. SDT other and EIT other
    go. The NIT, BAT, TDT, and TOT pass unchanged.
  - Follows PAT and PMT changes during the session. A per-program track ends
    when its program leaves the PAT. An unmodified-program track ends when the
    PAT no longer lists its program alone. When the change alters initData,
    `LivePipeline::updateCatalog` publishes a new independent catalog before
    the Object that carries the change (MSF -01 Section 5.3 forbids changing
    a declared track). The other catalog fields keep their values from the
    start.

- `src/media/LibavCaptureSource.*`
  - Optional direct libavdevice capture path.
  - Uses libavcodec for H.264/AAC/Opus encoding and libavformat for MPEG-TS
    muxing.
  - Emits the same `M2tsObject` shape as the file packetizer.
  - Can emit preview `QImage` frames and audio RMS levels from the same decoded
    input frames used for encoding.

- `src/media/LibavPreviewWorker.*`
  - Preflight capture preview worker.
  - Opens selected libavdevice camera/microphone inputs without starting a
    publisher session.
  - Selects the same V4L2 node/pixel format as the publish path via
    `resolveCaptureOpen`, so the preview opens the broadcast-rate MJPEG node
    rather than a raw lower-rate node.
  - Emits decoded video frames and left/right audio levels to `PreviewPanel`.

- `src/media/V4l2Capabilities.*` (Linux only)
  - Pure `selectBestMode(modes, w, h, fps)` chooses the best `(node, format)`:
    prefers an MJPEG mode meeting the target, then raw meeting the target, then
    best-effort highest fps at the requested size. Unit-tested in
    `tests/v4l2_selection_test.cpp` (no device required).
  - `queryModes(node)` enumerates a node's discrete modes via V4L2 ioctls;
    `groupNodesForCamera(node)` groups capture nodes that share a sysfs USB
    parent (one physical camera can expose several `/dev/videoN` nodes).
  - `resolveCaptureOpen(cameraDeviceId, w, h, fps)` composes those into the
    single `{node, useMjpeg}` decision shared by capture and preview.

- `src/media/MsftsMuxer.*`
  - Generates a compact MSF catalog for the `mpeg2ts` packaging value.
  - Adds an optional MSF `mediatimeline` side-track catalog entry.

- `src/publish/MoqxrPublisher.*`
  - Provides `IMoqOutput` interface.
  - Mock implementation logs payload sends.
  - `MOQ2TS_HAS_MOQXR` compile section is the hook for real SDK wiring.

- `CMakeLists.txt`
  - UI + dependencies + optional feature toggles.

## Building and dependency matrix

| Feature        | Toggle | Required to enable | Code path |
|----------------|--------|-------------------|----------|
| Mock publisher | `MOQ2TS_BUILD_WITH_MOCK_MOQXR` | none | default ON |
| Device listing | auto-detected | libavdevice/platform APIs or Qt Multimedia fallback | UI enumeration only |
| Device capture | auto-detected | libavdevice/libswscale/libswresample | camera/mic to MPEG-TS |
| OpenH264 path  | `MOQ2TS_ENABLE_OPENH264`     | libopenh264          | encoder integration |
| libav path     | `MOQ2TS_ENABLE_LIBAV_AUDIO`  | libavcodec/libavformat | media integration |
| libopus path   | `MOQ2TS_ENABLE_LIBOPUS`      | libopus              | Opus audio path |

### Bookworm build modes

The Bookworm script defaults to mock publisher mode:

```bash
./scripts/build-debian-bookworm.sh
```

Mock publisher mode is for local pipeline testing and only accepts endpoints
starting with `mock://`, such as `mock://local`. A real relay URL in a mock build
is rejected with an explicit error.

To publish to a real relay, build against the sibling `../moqxr` checkout:

```bash
MOQ2TS_BUILD_WITH_MOCK_MOQXR=OFF ./scripts/build-debian-bookworm.sh
```

The script runs the container as the current user and mounts `../moqxr` read-only
when the sibling checkout exists. Build output should remain owned by the current
user under `build-bookworm`.

## Integrating the real moqxr API

The app uses the local moqxr `Publisher::publish_live_objects` API. The adapter
builds a `LiveObjectSource` with:

- a `catalog` track carrying the MSF catalog JSON
- one `mpeg2ts` media track carrying source-packet object payloads
- caller-supplied Group ID, Subgroup ID, Object ID, media time, and duration
- payloads without CMSF/CMAF parsing

Suggested mapping:

- `connect(cfg)` -> store endpoint, namespace, and stream settings
- `publishLiveObjects(...)` -> call `openmoq::publisher::Publisher::publish_live_objects`
- `stop()` -> `disconnect(0)` -> `MoqtSession::close(0)`, which sets an atomic
  stop flag the publish loop observes so a running publish winds down promptly
  (with a bounded graceful flush) instead of blocking. `LivePipeline` waits for
  its worker with a bounded join plus a detach fallback, and the GUI-thread
  teardown is bounded, so pressing Stop never freezes the UI. (Implemented in
  the sibling `../moqxr` `MoqtSession` plus `LivePipeline`/`main.cpp` here.)

The real-linked path is compiled only when `MOQ2TS_BUILD_WITH_MOCK_MOQXR=OFF`.
With the default mock build, `MoqxrPublisher::connect()` accepts only
`mock://...` endpoints. This avoids confusing real relay URLs with successful
local mock sessions.

## Draft MSFTS conformance notes

The media Object payload is only consecutive source packets. Do not prepend
private headers. Object payload length must be a multiple of
`mpeg2tsPacketSize`.

`initData` is Base64 of whole source packets. For 188-byte TS this means PAT
and PMT packets beginning with sync byte `0x47`; for 192-byte M2TS this means
the four-byte timestamp prefix is retained and sync byte `0x47` remains at
offset 4.

For an MPTS input, `mpeg2tsProgramNumber` identifies the selected program. The
published media track is program-filtered; it is not a raw pass-through of every
PID in the source multiplex.

## Catalog shape

`MsftsMuxer::catalogJson` emits a compact MSF catalog (`draft-ietf-moq-msf-01`
common fields plus `draft-gregoire-moq-msfts` mpeg2ts fields). This encoder
produces the `unmodified-program`, `unmodified-multiplex`, and `per-program`
values of the required `mpeg2tsMode` field, and never `es-packets`. A live
capture catalog (`per-program`) looks like:

```json
{
  "version": "draft-01",
  "generatedAt": 1779416505123,
  "tracks": [
    {
      "name": "program-1",
      "packaging": "mpeg2ts",
      "isLive": true,
      "role": "video",
      "mimeType": "video/mp2t",
      "targetLatency": 1000,
      "mpeg2tsPacketSize": 188,
      "mpeg2tsMode": "per-program",
      "mpeg2tsProgramNumber": 1,
      "mpeg2tsPcrPid": 256,
      "mpeg2tsRandomAccess": true,
      "initRef": "init-program-1"
    },
    {
      "name": "program-1.timeline",
      "packaging": "mediatimeline",
      "depends": ["program-1"],
      "mimeType": "application/json"
    }
  ],
  "initDataList": [
    { "id": "init-program-1", "type": "inline", "data": "R0AAEAAAs...==" }
  ]
}
```

An `--unmodified` track is `unmodified-program` when the source PAT lists one
program. It then carries `mpeg2tsProgramNumber` and `mpeg2tsPcrPid` like a
`per-program` track. Otherwise it is `unmodified-multiplex`. Its
`mpeg2tsProgramNumber` and `mpeg2tsPcrPid` then name a reference program,
the first of the PAT or the `--program` one, on whose PCR a subscriber paces
the multiplex. It never carries `mpeg2tsMuxRate` or `mpeg2tsSiPids`.

Field presence is conditional:

- Per-track `namespace` is emitted only when non-empty.
- `generatedAt` is included only for live streams (MSF 5.1.6).
- `targetLatency` is present only when `isLive` is true (MSF 5.1.16); `trackDuration`
  is the inverse — VOD only, present only when `isLive` is false and the value is
  positive (MSF 5.1.37).
- `bitrate` is emitted only when greater than zero.
- `mpeg2tsProgramNumber` and `mpeg2tsPcrPid` are emitted in every mode when
  `mpeg2tsPcrPid` is known (PID >= 0), and always outside
  `unmodified-multiplex`. `mpeg2tsMuxRate` is emitted outside
  `unmodified-multiplex` when positive.
- `mpeg2tsSiPids` is emitted only in `per-program` mode.
- `mpeg2tsTimestampMode` is valid only for 192-octet source packets and MUST
  NOT appear for 188.
- `mpeg2tsRandomAccess` is advertised only when the first Object of every
  Group contains a random access point. A live source (pipe, FIFO, SRT)
  drops the packets before its first random access point and declares it,
  if the source sets the random_access_indicator on its video PID (or on the
  PCR PID without video). On `unmodified-multiplex`, these are the points of
  the reference program, and a multiplex without one declares nothing. A
  file keeps byte 0 and does not declare it. Groups longer than 2 seconds
  give one warning.

## Timeline track

The timeline track is separate from the M2TS media track so media object payloads
remain draft-MSFTS-clean. It is an MSF media timeline track
(`draft-ietf-moq-msf-01` Section 7.2): MSF `packaging: "mediatimeline"`, a `depends`
list naming the media track(s) it applies to, and an `application/json` MIME
type (see the catalog example above).

Timeline object payloads are an MSF media timeline (Section 7.1): a JSON array of
records, where each record is a three-item array

```json
[[ 12000, [0, 48], 1779416505123 ]]
```

with the items being `mediaPresentationTimeMs`, `[groupId, objectId]`, and
`wallclockMs` (milliseconds since the Unix epoch), each the floor in integral
milliseconds (MSF Section 7.1.1).

`LivePipeline` adds one record per media Group. The record points at the first
Object of the Group that starts a video PES, and carries that PES's PTS. The
capture path reads the PTS from its own muxed bytes. The wallclock time is the publish
time on a live source and `0` on a VOD file.

MSF Section 7.3 makes the first Object of each timeline Group an independent
timeline. The timeline track therefore publishes one Object per media Group, in
the Group of the same number, and that Object carries every record so far. The
history grows for the length of a session, so moq2ts suits limited-time runs
until MSF defines how to trim it (moq-wg/msf#205).

## Runtime operational guidance

- Keep both audio and video sources timestamped from a single master clock if
  low-latency sync matters.
- If using separate files, treat as asynchronous tracks and apply track-level
  offset compensation at the sender.
- For true live capture, replace file-based source URIs with capture endpoints.
- Use the Preview tab before publishing to verify that the selected camera and
  microphone are readable. During capture publishing, the Preview tab uses the
  same decoded source frames and samples as the encoder path.
- Normal speech should move the audio meters. The UI maps normalized RMS to a
  `-60 dBFS` to `0 dBFS` display range instead of showing raw linear samples.

## Known limitations in this scaffold

- Audio/video synchronization is intentionally simplified.
- Capture device naming is platform-specific. Linux uses `v4l2` for camera IDs
  and PulseAudio device names for microphones; Windows uses DirectShow-style
  names; macOS uses AVFoundation names.
- The sample MSFTS frameizer is draft-oriented and not the authoritative final
  wire format.
- Program filtering preserves source PAT/PMT packet bytes in `initData`; it does
  not rewrite PAT sections to remove other program entries yet.
- No retransmission/back-pressure strategy is included yet.
- No auth/token refresh path is implemented in the publisher adapter.
