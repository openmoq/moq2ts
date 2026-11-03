#pragma once

#include <QSet>
#include <QString>

#include <atomic>
#include <memory>

#include "../media/SiTables.h"

namespace moq2ts {

class SourceGapLog;

enum class AudioCodecPreset {
    AAC,
    Opus,
};

struct PublishConfig {
    QString moqEndpoint = "mock://local";
    QString namespaceName = "live/paul1";

    QString videoSource = QString(); // M2TS file/pipe/URL for H.264 source
    QString audioSource = QString(); // M2TS file/pipe/URL for audio source (optional)
    QString cameraDeviceId = QString(); // UI-selected capture device id
    QString microphoneDeviceId = QString(); // UI-selected capture device id

    int videoWidth = 1920;
    int videoHeight = 1080;
    int videoFramerate = 30;
    int videoTargetBitrateKbps = 2500;
    int keyframeIntervalMs = 1000;  // live H.264 IDR cadence; MOQT group cadence follows it
    bool paceEgress = true; // pace capture object release on real media time

    int audioSampleRate = 48000;
    int audioChannels = 2;
    int audioTargetBitrateKbps = 160;
    AudioCodecPreset audioCodec = AudioCodecPreset::AAC;

    int fragmentDurationMs = 250;
    int targetSegmentBytes = 64 * 1024;
    int programNumber = 0; // 0 selects the first nonzero PAT program

    // MSFTS carriage-profile options (draft-gregoire-moq-msfts feedback, msfts#7).
    // Defaults preserve the historical filtered single-program behavior.
    bool transparentMode = false;   // carry the whole multiplex verbatim (no filter/rewrite)
    // filtered path: the SI tables to keep (--si, or all of them for
    // --retain-si); none when empty.
    QSet<SiTable> siTables;
    bool retainNullPackets = false; // filtered path: also keep null (0x1FFF) packets
    int mpeg2tsMuxRateBps = 0;      // advisory source mux rate; 0 omits the catalog hint
    bool pacedFileSource = false;    // pace file-source publishing at media-time rate
    int draftVersion = 22;              // MOQT transport draft version
    bool insecure = false;              // skip verifying the relay certificate (--insecure)
    // Set by the pipeline: the source is live (capture, FIFO, pipe, or SRT),
    // so the publish waits for subscribers for as long as it runs, and a
    // session that ends before the source does is an error.
    bool liveSource = false;
    // Set by the pipeline to the PID whose random access points start Groups;
    // the SRT ring buffer resyncs on it after a drop. Null outside SRT ingest.
    std::shared_ptr<std::atomic<int>> resyncPid;
    // Where the SRT ring buffer skipped ahead in the bytes it wrote; the
    // packetizer starts a Group at each gap. Null outside SRT ingest.
    std::shared_ptr<SourceGapLog> sourceGaps;

    bool forceRealtime = true;
    bool useOpenh264 = true;
    bool useLibAvTranscode = true;
    bool useLibOpusFallback = false;
};

} // namespace moq2ts
