# moq2ts — MOQT MPEG-TS publisher and receiver

[![Build](https://github.com/openmoq/moq2ts/actions/workflows/build.yml/badge.svg)](https://github.com/openmoq/moq2ts/actions/workflows/build.yml)

This repository contains a Qt-based application that publishes MPEG-2 transport
streams over MOQT using the moqxr publisher SDK.
It also contains a console receiver built on moq5 main that subscribes to an
MSF MPEG-TS track over MOQT draft 22, writes source packets, and prints live
transport and delivery statistics.
The media pipeline accepts TS or M2TS input and produces
`draft-gregoire-moq-msfts` media objects: each object payload is a
concatenation of whole 188-byte TS packets or 192-byte M2TS source packets.
The checked-in MSFTS reference is [draft 02](docs/draft-gregoire-moq-msfts-02.txt).
The catalog follows checked-in [MSF draft 01](docs/draft-ietf-moq-msf-01.txt).
The `-00` MSFTS and CMSF text files and dated `docs/superpowers` plans/specs
are retained as historical references. With moqxr v0.4.4, this adapter supports
MOQT transport drafts 14, 16, 17, 18, 21, and 22, and defaults to draft 22.

The application targets:

- H.264 video encoding via `openh264`
- AAC or Opus audio at 48kHz stereo via direct libav/libopus integration points
- M2TS stream ingestion for live audio/video paths
- Cross-platform desktop UI (Qt Widgets)
- CMake build with a Debian Bookworm Docker build entrypoint

## Prebuilt binaries

GitHub Actions builds Linux, macOS, and Windows packages on every push and pull
request (see the [Build workflow](.github/workflows/build.yml)):

- **Linux** — `moq2ts-publisher-linux-x86_64.AppImage` (self-contained; `chmod +x`
  and run) and `moq2ts-cli-linux-x86_64.AppImage`, the command-line publisher
- **macOS** — `moq2ts-publisher-macos.zip` (a bundled `.app`; the command-line
  publisher is `moq2ts-publisher.app/Contents/MacOS/moq2ts-cli`)
- **Windows** — `moq2ts-publisher-windows.zip` (the `.exe` and `moq2ts-cli.exe`
  with their Qt and codec DLLs). The Windows build is off by default: start the
  workflow by hand with `windows` set, or set the repository variable
  `BUILD_WINDOWS` to `true`. A release without it has no Windows asset.

The receiver (`moq2ts-receiver`) is not packaged: it needs an installed moq5,
which has no prebuilt SDK yet (see [docs/CLI.md](docs/CLI.md#receive-mpeg-ts-from-a-relay)).

Per-commit packages are attached to each run on the Actions tab. Tagged releases
(`v*`) additionally publish these assets to a GitHub Release.

These CI binaries are **real, relay-capable builds**: each links the prebuilt
openmoq publisher SDK from the public `openmoq/moqxr` releases (pinned via the
`MOQXR_VERSION` workflow variable) together with the full capture and transcode
stack (Qt6 + ffmpeg/libav + openh264 + opus), so they can publish to a real MOQT
relay endpoint. The SDK ships static libraries (publisher + picoquic/picotls);
OpenSSL is linked from the platform. On Windows the build uses MSVC + vcpkg to
match the SDK's ABI; Linux and macOS use the system GCC/Clang toolchains.

A mock build (UI, file packetization, and local object generation with `mock://`
endpoints only) is available locally with
`-DMOQ2TS_BUILD_WITH_MOCK_MOQXR=ON`. See [moqxr integration](#moqxr-integration)
for linking the SDK (`MOQXR_SDK_DIR`) or a local `../moqxr` source checkout.

To cut a release, push a version tag:

```bash
git tag v0.1.0
git push origin v0.1.0
```

## Project layout

- `src/main.cpp`: Qt application bootstrap and signal wiring
- `src/app/*`: UI and publish configuration model
- `src/media/*`: media pipeline, capture preview, TS/M2TS packetizer, and draft MSFTS catalog helpers
- `src/publish/*`: MOQT publisher adapter (moqxr `publish_live_objects` and a local mock path)
- `src/receive/*`: moq5-based MPEG-TS receiver CLI and transport-stream counters
- `docker/*`: Bookworm container definition
- `scripts/*`: build automation
- `docs/*`: sequence diagrams and developer documentation

## UI feature set

- Live configuration of endpoint/namespace/stream
- Separate video and audio source selectors (M2TS file/pipe)
- Camera and microphone enumeration through libavdevice/platform capture APIs,
  with Qt Multimedia fallback where enabled
- Preview tab with decoded video preview and dB-scaled left/right audio meters
- In-process libavdevice capture path for selected camera/microphone devices
- M2TS program selection for publishing one program/track from an MPTS source
- MSF timeline side track for wall-clock correlation
- Tunable codec and bitrate settings
- Fragment size control and pacing
- Status/log output and basic counters

## Build prerequisites

- CMake >= 3.24
- Qt Widgets (Qt 5 or Qt 6)
- Optional (recommended for real encode path):
  - `libopenh264-dev`
  - `libav*` dev packages
  - `libavdevice-dev`
  - `libswscale-dev`
  - `libopus-dev`

The default build requires moqxr v0.4.4 or newer publisher headers and
libraries, either from a sibling `../moqxr` checkout or `MOQXR_SDK_DIR`. Set
`MOQ2TS_BUILD_WITH_MOCK_MOQXR=ON` explicitly for a local mock build; that build
only accepts `mock://...` endpoints.

## Build locally

```bash
cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
```

Run:

```bash
./build/moq2ts-publisher
```

In the UI, replace the initial `mock://local` value with your relay's
`moqt://host:port` or `https://host:port/path` URL to publish over MOQT. For
local pipeline testing, configure a separate build with
`-DMOQ2TS_BUILD_WITH_MOCK_MOQXR=ON` and keep `mock://local`.

## Receive from a relay

The console receiver uses moq5 and needs no Qt runtime. Build it against an
installed moq5 main build with the `service` and `mpeg2ts` targets enabled:

```bash
cmake -S src/receive -B build-receiver -DCMAKE_PREFIX_PATH=/path/to/moq5/install
cmake --build build-receiver -j
./build-receiver/moq2ts-receiver \
  --url moqt://127.0.0.1:4433 --namespace live/ch1 \
  --out capture.ts --duration 10 --insecure
```

Use `--insecure` only with a local or test relay whose certificate is not
trusted. The receiver selects an MPEG-TS track from the MSF catalog, saves its
source packets, and prints a stats line each second. Use `--track NAME` when
the catalog has multiple matching tracks.

![Terminal output from a local draft 22 receiver run, including packet counts and reported MoQT gaps](docs/receiver-cli.svg)

This capture from a local Red5 relay shows five reported MoQT gaps; its zero
`sync`, `malformed`, `tei`, and `cc` counters describe the TS packets that
arrived. See the [CLI guide](docs/CLI.md#receive-mpeg-ts-from-a-relay) for all
receiver options, counter meanings, and the end-to-end test.

## Docker (Debian Bookworm)

```bash
./scripts/build-debian-bookworm.sh
```

The script builds a Bookworm container image, compiles the app, and installs
artifacts under `build-bookworm/install`.

The default Bookworm build links the sibling `../moqxr` checkout for real MOQT
publishing. The script mounts that checkout into the container when present.
For local capture, muxing, preview, and object-generation testing without a
relay, choose the mock build explicitly:

```bash
MOQ2TS_BUILD_WITH_MOCK_MOQXR=ON ./scripts/build-debian-bookworm.sh
```

Launch the packaged app with:

```bash
./build-bookworm/install/moq2ts-publisher-bookworm
```

## Notes on codec paths

- **MSFTS packaging**: media object payloads are raw source packets only. The app
  does not add a private wrapper before publication.
- **Packet size**: source packets are validated as either 188-byte TS or 192-byte
  M2TS packets, with sync byte checks required by the draft.
- **Catalog**: the media track uses `packaging: "mpeg2ts"` with MSF common
  fields (`isLive`, `role`, `mimeType`, `targetLatency`) and MSFTS mpeg2ts
  fields (`mpeg2tsMode`, `mpeg2tsPacketSize`, `mpeg2tsProgramNumber` and
  optional `mpeg2tsPcrPid` on the `per-program` and `unmodified-program`
  modes, and `mpeg2tsRandomAccess` when the first Object of every Group
  contains a random access point).
  `mpeg2tsTimestampMode` is emitted only for 192-octet source packets.
  `mpeg2tsMuxRate` appears only for derived `per-program` tracks; SRT tracks
  omit `mpeg2tsRandomAccess` because recovery can start without a keyframe.
- **Initialization data**: the packetizer scans the start of the source for
  PAT and PMT packets and places them in a Base64 `initDataList` entry referenced
  by the media track's `initRef`, preserving source-packet form.
- **Program filtering**: in `per-program` mode, the selected program is discovered from PAT/PMT.
  Media objects include a PAT rewritten to list that program only, the
  selected PMT, the selected PCR PID, and the elementary PIDs for that
  program. Packets for other programs and null packets are dropped before
  publication.
- **Timeline track**: each media track gets a `<stream>.timeline` side track of
  MSF `packaging: "mediatimeline"` that `depends` on the media track. Timeline objects
  are a compact `[mediaTimeMs, [groupId, objectId], wallclockMs]` record array
  mapping media presentation time to Unix wall-clock milliseconds without
  modifying M2TS payloads.
- **Encoding**: the in-process capture path uses libavcodec/libavformat to
  encode and mux 188-byte MPEG-TS packets. The file, pipe, and SRT paths take
  an existing TS/M2TS stream; the application does not invoke `ffmpeg`.
- **Capture devices**: camera and microphone devices are listed in the UI through
  libavdevice/platform capture APIs, with Qt Multimedia fallback where enabled.
  If no file/pipe source is provided, selected devices are opened through
  libavdevice, encoded in-process, muxed to MPEG-TS through libavformat, and
  published as MSFTS MPEG-TS objects. On Linux the capture path probes the camera's
  V4L2 modes and prefers an MJPEG mode at the requested resolution/framerate
  (so e.g. 1080p30 is honored where raw YUYV would be capped), reconciling the
  encoder to the framerate the driver actually negotiates. Transient corrupt
  decode frames are skipped rather than aborting capture.
- **Preview**: the Preview tab can open selected camera/microphone devices before
  publishing. During live capture publishing, the Preview tab is driven from the
  same decoded frames and audio samples used by the encoder path. Audio meters
  display RMS levels on a dBFS scale so normal speech is visible.

## moqxr integration

`src/publish/MoqxrPublisher.cpp` wires the MSFTS object stream into the moqxr
`Publisher::publish_live_objects` API. There are two ways to link the real
publisher (both require OpenSSL on the system).

Link a prebuilt SDK from the `openmoq/moqxr` releases (what CI uses) — extract the
archive for your platform and point `MOQXR_SDK_DIR` at it:

```bash
cmake -S . -B build \
  -DMOQXR_SDK_DIR=/path/to/extracted-sdk \
  -DMOQ2TS_BUILD_WITH_MOCK_MOQXR=OFF
```

Or build against a neighboring moqxr source checkout:

```bash
cmake -S . -B build \
  -DMOQXR_SOURCE_DIR=../moqxr \
  -DMOQ2TS_BUILD_WITH_MOCK_MOQXR=OFF
```

Mock mode remains available with `MOQ2TS_BUILD_WITH_MOCK_MOQXR=ON`.

Mock mode is explicit: endpoints must start with `mock://`. Entering a real
relay URL in a mock build fails with an error instead of pretending to connect.

## Sequence diagrams

See [`docs/sequence-diagrams.md`](docs/sequence-diagrams.md).

## Developer docs

See [`docs/DEVELOPER.md`](docs/DEVELOPER.md).

## Attribution

Originally created by [Paul Gregoire](https://github.com/mondain) as an example for the [msfts](https://datatracker.ietf.org/doc/draft-gregoire-moq-msfts/) draft.

