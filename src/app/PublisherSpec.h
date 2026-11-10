#pragma once

#include <QByteArray>
#include <QList>
#include <QString>

#include "../media/MptsPipeline.h"
#include "PublishConfig.h"

namespace moq2ts {

// SRT ingest as the configuration states it; the CLI maps it to
// SrtSource::Config, so this file builds without libsrt.
struct SrtIngestSpec {
    // "listener" or "caller". Empty when the ingest is a path.
    QString mode;
    QString host;
    int port = 0;
    int latencyMs = 0;
};

// One multi-program publisher: the relay session, one live source, and the
// programs published as per-program tracks (draft "Per-Program").
struct PublisherSpec {
    // The relay settings: moqEndpoint, namespaceName, draftVersion, and
    // insecure, and the media timeline: timelineTrack and accessibleWindowMs.
    PublishConfig relay;
    // Exactly one ingest is set: srt (srt.mode not empty) or sourcePath (a FIFO).
    SrtIngestSpec srt;
    QString sourcePath;
    QList<MptsProgramConfig> programs;
    int segmentBytes = 64 * 1024;
};

// Reads a publisher configuration:
//   {
//     "relay": { "endpoint": "moqt://relay.example:4433", "namespace": "live/mpts", "draft": 22,
//                "insecure": false },
//     "ingest": { "srt": { "mode": "listener", "host": "0.0.0.0", "port": 9000, "latency_ms": 200 } },
//     "programs": [ { "number": 101, "mux_rate_bps": 6000000, "si": ["sdt", "eit-pf", "tdt-tot"] },
//                   { "number": 102 } ],
//     "segment_bytes": 65536,
//     "timeline": true, "accessible_window_ms": 300000
//   }
// "ingest" holds exactly one of "srt" or "path" (a FIFO). An unknown key
// fails. On failure, error names the field and the value, and out is left
// unchanged.
bool parsePublisherSpec(const QByteArray& json, PublisherSpec* out, QString* error);

} // namespace moq2ts
