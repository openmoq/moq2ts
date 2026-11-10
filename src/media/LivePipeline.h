#pragma once

#include "media/EgressPacing.h"
#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <QImage>
#include <QObject>

#include "../app/PublishConfig.h"
#include "../publish/MoqxrPublisher.h"
#include "M2tsPacketizer.h"
#include "MsftsMuxer.h"

namespace moq2ts {

// MSF media timeline history of a track (draft-ietf-moq-msf-01 Section 7.3):
// the records inside the accessible window, and the last media Group recorded.
struct TimelineHistory {
    struct Record {
        // The steady clock in ms when the record was added: what the
        // accessible window is measured on, so that a wallclock step does not
        // move it.
        std::int64_t timeMs = 0;
        QByteArray json;
    };
    std::deque<Record> records;
    std::optional<std::uint64_t> lastGroupId;
};

// State the nextObject lambda mutates while publishing.
//
// This is heap-allocated and captured BY VALUE (as a shared_ptr) rather than left on
// runLoop()'s stack and captured by reference. That matters because the lambda is
// handed to the publisher and invoked from moqxr's own publish thread, while
// waitForStopped() gives the worker 3 seconds and then DETACHES it. If the worker's
// frame unwinds while moqxr still holds the lambda, by-reference captures point at
// reclaimed stack and writing through them corrupts the heap -- observed as
// "double free or corruption" and "corrupted size vs. prev_size" at shutdown, and
// correlated with a stalled SRT source, which is what makes the 3 s deadline slip.
//
// Sharing ownership means the state simply outlives whichever side finishes last.
struct PublishState {
    std::int64_t objects = 0;
    std::int64_t bytes = 0;
    TimelineHistory timeline;
    std::int64_t pacingStartUs = -1;
    // --paced follows the PCR of the source once it has shown one; before that
    // (or without one) it falls back to fragment-duration steps.
    PcrClock pcrClock;
    bool pacingOnPcr = false;
    // Objects to return before reading the next one, in order.
    std::deque<PublishedObject> pending;
    // The catalog as published, and the group of its last catalog object.
    MsftsCatalog catalog;
    std::uint64_t catalogGroupId = 0;
    // The catalog that ends a live broadcast has gone out.
    bool broadcastEnded = false;
    // The packetizer ended the track because the source PAT changed, and the
    // pipeline starts a session with a new track.
    bool restart = false;
    std::shared_ptr<M2tsPacketizer> packetizer;   // file / SRT path
    std::shared_ptr<void> capture;                // capture path (LibavCaptureSource)
};

// Adds the record of a media Group to history and returns Object 0 of the
// timeline Group of the same number, which holds every accessible record (MSF
// -01 Section 7.3). Records added more than windowMs of steadyUs before the
// newest one are dropped, as the editor's copy of MSF asks publishers to trim
// the timeline to the accessibleWindowTime; 0 keeps every record. The Object names the media
// track as its afterTrack, so the publisher sends it after the media Object it
// records.
PublishedObject timelineObject(TimelineHistory* history,
                               const QString& timelineTrackName,
                               const PublishedObject& media,
                               std::uint64_t mediaTimeUs,
                               std::uint64_t wallClockUnixUs,
                               std::int64_t steadyUs,
                               std::int64_t windowMs = 0);

// The wall clock, in microseconds since the Unix epoch, for the records of a
// live track.
std::uint64_t nowUnixUs();

// Completes the catalog entry of a track read by packetizer with the fields
// the stream gives: packet size, program, PCR PID, SI PIDs, timestamp mode,
// initData, random access, and the timeline track <track>.timeline. entry
// carries the fields that depend on the pipeline: track, packetsPerObject,
// mode, rate, bitrate, liveness, and duration.
MsftsCatalog packetizerCatalog(const M2tsPacketizer& packetizer, MsftsCatalog entry, bool withTimeline = true);

class LivePipeline final : public QObject {
    Q_OBJECT
public:
    explicit LivePipeline(QObject* parent = nullptr);
    ~LivePipeline() override;

    void start(const PublishConfig& cfg, IMoqOutput* publisher);
    void stop();
    void requestStop();
    void waitForStopped();
    bool running() const;

signals:
    void status(const QString& message);
    void error(const QString& message);
    void stats(int64_t packets, int64_t bytes, int64_t groups);
    void previewVideoFrame(const QImage& image);
    void previewAudioLevels(double left, double right);

private:
    void runLoop();

    PublishConfig m_config;
    IMoqOutput* m_publisher = nullptr;

    std::atomic<bool> m_running {false};
    std::thread m_workerThread;

    std::mutex m_doneMutex;
    std::condition_variable m_doneCv;
    bool m_workerDone = false;
};

} // namespace moq2ts
