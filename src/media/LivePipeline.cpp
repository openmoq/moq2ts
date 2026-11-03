#include "LivePipeline.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <thread>
#include <QDateTime>
#include <QDebug>
#include <QThread>

#include "EgressPacing.h"
#include "LibavCaptureSource.h"
#include "MsftsMuxer.h"

namespace moq2ts {

std::uint64_t nowUnixUs() {
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(now).count());
}

namespace {

// Latency slack (us) before an object is considered due. Small so steady-state
// latency stays near zero; only objects arriving ahead of their media time wait.
constexpr std::int64_t kPaceSlackUs = 8000;

// The MSF bitrate of a TS-source track (MSF -01 Section 5.2.22): its maximum
// bitrate, which MUST be given for a video track. A TS source is not encoded
// here, so the value comes from the stream, in order of certainty:
//   1. the measured rate, when it is constant: an unmodified track of a source
//      with null stuffing carries a constant rate, so its rate is its maximum;
//   2. the mux rate, --mux-rate or the one measured for a single-program
//      source: no track derived from the source exceeds it;
//   3. the rate measured over the first second, an average that a VBR track
//      can exceed;
//   4. the --video-bitrate setting, which only the capture path encodes to.
qint64 trackMaxBitrate(const M2tsPacketizer& packetizer, const PublishConfig& config) {
    if (packetizer.measuredBitrate() > 0 && packetizer.measuredBitrateIsConstant()) {
        return packetizer.measuredBitrate();
    }
    if (config.mpeg2tsMuxRateBps > 0) {
        return config.mpeg2tsMuxRateBps;
    }
    if (packetizer.measuredMuxRate() > 0) {
        return packetizer.measuredMuxRate();
    }
    if (packetizer.measuredBitrate() > 0) {
        qWarning("The catalog bitrate is the rate measured over the first second of the source; the track can "
                 "exceed it, and MSF declares a maximum. Give --mux-rate to declare an upper bound.");
        return packetizer.measuredBitrate();
    }
    qWarning("The catalog bitrate is the --video-bitrate setting: no rate could be measured from the PCR. "
             "Give --mux-rate to declare the rate of the source.");
    return static_cast<qint64>(config.videoTargetBitrateKbps) * 1000;
}

// The one place where a PSI change reaches the catalog: a new independent
// catalog with the updated initialization data, as MSFTS 6.12 asks. A delta
// update could not carry it: MSF -01 Section 5.3 allows only adding, cloning,
// or removing a track. Switch to the "update" operation once MSF -02 and the
// SDK support it (moq-wg/msf#183).
PublishedObject updateCatalog(PublishState* st, const QByteArray& initData) {
    st->catalog.initData = initData;
    if (st->catalog.isLive) {
        st->catalog.generatedAtMs = QDateTime::currentMSecsSinceEpoch();
    }
    PublishedObject catalog;
    catalog.trackName = QLatin1String(kCatalogTrackName);
    catalog.payload = MsftsMuxer::catalogJson(st->catalog);
    catalog.groupId = ++st->catalogGroupId;
    catalog.objectId = 0;
    catalog.startsGroup = true;
    catalog.finalInGroup = true;
    return catalog;
}

// Once a live source ends, the independent catalog that ends the broadcast
// for good (MSF -01 Section 11.3). Section 11.3 sends PUBLISH_DONE with Track
// Ended first: moqxr holds an object marked endsBroadcast until it has ended
// every other track. A VOD track gets nothing more.
std::optional<PublishedObject> endBroadcast(PublishState* st) {
    if (!st->catalog.isLive || st->broadcastEnded) {
        return std::nullopt;
    }
    st->broadcastEnded = true;
    st->catalog.generatedAtMs = QDateTime::currentMSecsSinceEpoch();
    PublishedObject catalog;
    catalog.trackName = QStringLiteral("catalog");
    catalog.payload = MsftsMuxer::completeCatalogJson(st->catalog);
    catalog.groupId = ++st->catalogGroupId;
    catalog.objectId = 0;
    catalog.startsGroup = true;
    catalog.finalInGroup = true;
    catalog.endsBroadcast = true;
    return catalog;
}

} // namespace

PublishedObject timelineObject(TimelineHistory* history,
                               const QString& timelineTrackName,
                               const PublishedObject& media,
                               std::uint64_t mediaTimeUs,
                               std::uint64_t wallClockUnixUs) {
    if (!history->records.isEmpty()) {
        history->records += ',';
    }
    history->records += MsftsMuxer::mediaTimelineRecord(mediaTimeUs, media.groupId, media.objectId, wallClockUnixUs);
    history->lastGroupId = media.groupId;

    PublishedObject timeline;
    timeline.trackName = timelineTrackName;
    timeline.payload = '[' + history->records + ']';
    timeline.groupId = media.groupId;
    timeline.objectId = 0;
    timeline.finalInGroup = true;
    timeline.mediaTimeUs = media.mediaTimeUs;
    timeline.mediaDurationUs = 0;
    timeline.afterTrack = media.trackName;
    return timeline;
}

MsftsCatalog packetizerCatalog(const M2tsPacketizer& packetizer, MsftsCatalog entry) {
    entry.packetSize = packetizer.packetSize();
    entry.programNumber = packetizer.programNumber();
    // A multiplex names a reference program only when --program gives one;
    // the catalog then carries no program fields.
    entry.pcrPid = packetizer.namesProgram() ? packetizer.pcrPid() : -1;
    // Advertised as mpeg2tsSiPids. Empty unless SI retention kept DVB SI PIDs
    // alongside the selected program, which is exactly the case the field
    // describes: tables retained in the filtered track beyond the PMT's list.
    entry.siPids = packetizer.retainedSiPids();
    // For 192-octet source packets the timestamp prefix is carried without
    // specified semantics ("opaque", draft-gregoire-moq-msfts); omitted for 188.
    entry.timestampMode = packetizer.packetSize() == 192 ? QStringLiteral("opaque") : QString();
    entry.initData = packetizer.initData();
    entry.timelineTrack = entry.track + QStringLiteral(".timeline");
    entry.randomAccess = packetizer.randomAccess();
    return entry;
}

LivePipeline::LivePipeline(QObject* parent)
    : QObject(parent) {}

LivePipeline::~LivePipeline() {
    stop();
}

bool LivePipeline::running() const {
    return m_running.load(std::memory_order_acquire);
}

void LivePipeline::start(const PublishConfig& cfg, IMoqOutput* publisher) {
    if (m_running.load(std::memory_order_acquire)) {
        emit status(QStringLiteral("Pipeline already running."));
        return;
    }

    if (!publisher) {
        emit error(QStringLiteral("No publisher instance provided."));
        return;
    }

    m_config = cfg;
    m_publisher = publisher;
    m_running.store(true, std::memory_order_release);

    const bool hasCaptureSource = !cfg.cameraDeviceId.isEmpty() || !cfg.microphoneDeviceId.isEmpty();
    if (cfg.videoSource.isEmpty() && cfg.audioSource.isEmpty() && !hasCaptureSource) {
        emit error(QStringLiteral("An M2TS source or capture device is required."));
        m_running.store(false, std::memory_order_release);
        return;
    }
    if (!cfg.videoSource.isEmpty() && !cfg.audioSource.isEmpty() && cfg.videoSource != cfg.audioSource) {
        emit error(QStringLiteral("draft-gregoire-moq-msfts carries a single ordered TS/M2TS packet stream per track. Use one multiplexed M2TS source for this scaffold."));
        m_running.store(false, std::memory_order_release);
        return;
    }

    {
        std::lock_guard<std::mutex> lock(m_doneMutex);
        m_workerDone = false;
    }
    m_workerThread = std::thread([this]() { runLoop(); });
    emit status(QStringLiteral("MSFTS M2TS packet pipeline started."));
}

void LivePipeline::stop() {
    const bool wasRunning = m_running.load(std::memory_order_acquire);
    requestStop();
    waitForStopped();

    if (wasRunning) {
        emit status(QStringLiteral("Streaming pipeline stopped."));
    }
}

void LivePipeline::requestStop() {
    m_running.store(false, std::memory_order_release);
}

void LivePipeline::waitForStopped() {
    if (!m_workerThread.joinable()) {
        return;
    }
    bool finished = false;
    {
        std::unique_lock<std::mutex> lock(m_doneMutex);
        finished = m_doneCv.wait_for(lock, std::chrono::seconds(3),
                                     [this]() { return m_workerDone; });
    }
    if (finished) {
        m_workerThread.join();
    } else {
        std::fprintf(stderr, "[moqxr][stop] worker join timed out after 3000ms; detaching\n");
        std::fflush(stderr);
        m_workerThread.detach();
    }
}

void LivePipeline::runLoop() {
    struct DoneSignal {
        LivePipeline* self;
        ~DoneSignal() {
            {
                std::lock_guard<std::mutex> lock(self->m_doneMutex);
                self->m_workerDone = true;
            }
            self->m_doneCv.notify_all();
        }
    } doneSignal{this};

    if (!m_publisher) {
        emit error(QStringLiteral("Pipeline is missing publisher."));
        m_running.store(false, std::memory_order_release);
        return;
    }

    if ((m_config.videoSource.isEmpty() && m_config.audioSource.isEmpty()) &&
        (!m_config.cameraDeviceId.isEmpty() || !m_config.microphoneDeviceId.isEmpty())) {
        // Heap-allocated and shared with the lambda; see PublishState in LivePipeline.h.
        auto capturePtr = std::make_shared<LibavCaptureSource>(m_config);
        LibavCaptureSource& capture = *capturePtr;
        capture.setPreviewCallbacks(
            [this](const QImage& image) {
                emit previewVideoFrame(image);
            },
            [this](double left, double right) {
                emit previewAudioLevels(left, right);
            });
        QString captureError;
        if (!capture.open(&captureError)) {
            emit error(captureError);
            m_running.store(false, std::memory_order_release);
            return;
        }

        const int packetsPerObject = std::max(1, m_config.targetSegmentBytes / capture.packetSize());
        // Track name defaults to "program-1" when not supplied by other means.
        const QString trackName = QStringLiteral("program-1");
        const QString timelineTrackName = trackName + QStringLiteral(".timeline");
        const MsftsCatalog catalogSpec{
            .track = trackName,
            .packetSize = capture.packetSize(),
            .packetsPerObject = packetsPerObject,
            .programNumber = capture.programNumber(),
            .pcrPid = capture.pcrPid(),
            .initData = capture.initData(),
            .timelineTrack = timelineTrackName,
            .randomAccess = capture.randomAccessActive(),
            .isLive = true,
            .bitrateBps = static_cast<qint64>(m_config.videoTargetBitrateKbps) * 1000,
            .generatedAtMs = QDateTime::currentMSecsSinceEpoch(),
        };
        const QByteArray catalog = MsftsMuxer::catalogJson(catalogSpec);

        m_config.liveSource = true;
        auto st = std::make_shared<PublishState>();
        st->catalog = catalogSpec;
        st->capture = capturePtr;   // keeps the source alive alongside the counters
        auto* capturePtrRaw = capturePtr.get();

        auto nextObject = [this, st, capturePtrRaw, packetsPerObject, trackName, timelineTrackName]() -> std::optional<PublishedObject> {
            if (!m_running.load(std::memory_order_acquire)) {
                return std::nullopt;
            }
            if (!st->pending.empty()) {
                PublishedObject next = std::move(st->pending.front());
                st->pending.pop_front();
                return next;
            }

            M2tsObject object;
            QString readError;
            if (!(*capturePtrRaw).readObject(packetsPerObject, &object, m_running, &readError)) {
                if (!readError.isEmpty()) {
                    emit error(readError);
                    return std::nullopt;
                }
                // Only a clean end of the source ends the broadcast: not an
                // error, and not an interrupt.
                return m_running.load(std::memory_order_acquire) ? endBroadcast(st.get()) : std::nullopt;
            }

            const bool startsGroup = object.startsGroup;

            PublishedObject published;
            published.trackName = trackName;
            published.payload = std::move(object.payload);
            published.groupId = object.groupId;
            published.objectId = object.objectId;
            published.startsGroup = startsGroup;
            published.mediaTimeUs = object.mediaTimeUs;
            published.mediaDurationUs = static_cast<std::uint64_t>(m_config.fragmentDurationMs) * 1000ULL;
            // One timeline record per media Group, for its first Object that
            // starts a video PES, with that PES's PTS (MSF 7.1.1).
            if (object.ptsUs.has_value() && st->timeline.lastGroupId != published.groupId) {
                st->pending.push_back(timelineObject(&st->timeline, timelineTrackName, published,
                                                     *object.ptsUs, nowUnixUs()));
            }
            ++st->objects;
            st->bytes += published.payload.size();
            emit stats(st->objects, st->bytes, static_cast<int64_t>(published.groupId + 1));
            if (m_config.paceEgress) {
                if (st->pacingStartUs < 0) {
                    // Anchor the pace clock so the first object is due immediately
                    // (subtract its media time), avoiding a one-time startup wait
                    // equal to the encode/mux buffering offset. Subsequent st->objects
                    // pace relative to this anchor on the same steady clock.
                    st->pacingStartUs = nowSteadyUs() - static_cast<std::int64_t>(published.mediaTimeUs);
                }
                while (m_running.load(std::memory_order_acquire)) {
                    const std::int64_t delay = paceDelayUs(static_cast<std::int64_t>(published.mediaTimeUs),
                                                           nowSteadyUs() - st->pacingStartUs, kPaceSlackUs);
                    if (delay <= 0) {
                        break;
                    }
                    std::this_thread::sleep_for(std::chrono::microseconds(std::min<std::int64_t>(delay, 5000)));
                }
            }
            return published;
        };

        if (!m_publisher->publishLiveObjects(m_config, trackName, QStringList{timelineTrackName}, catalog, std::move(nextObject), m_running)) {
            if (m_running.load(std::memory_order_acquire)) {
                emit error(QStringLiteral("Failed to publish captured MSFTS live object stream."));
            }
        }

        m_running.store(false, std::memory_order_release);
        emit status(QStringLiteral("Pipeline exiting."));
        return;
    }

    const QString sourcePath = !m_config.videoSource.isEmpty() ? m_config.videoSource : m_config.audioSource;
    // Heap-allocated and shared with the nextObject lambda below, NOT a stack local.
    // See the comment on PublishState in LivePipeline.h: the lambda outlives this
    // frame whenever waitForStopped() has to detach the worker, so anything it
    // touches has to outlive the frame too.
    auto packetizerPtr = std::make_shared<M2tsPacketizer>(sourcePath);
    M2tsPacketizer& packetizer = *packetizerPtr;
    // MSFTS carriage-profile knobs (msfts#7); must be set before open().
    packetizer.setTransparent(m_config.transparentMode);
    packetizer.setSiTables(m_config.siTables);
    packetizer.setRetainNullPackets(m_config.retainNullPackets);
    packetizer.setSourceGaps(m_config.sourceGaps);
    QString packetizerError;
    if (!packetizer.open(m_config.programNumber, &packetizerError)) {
        emit error(packetizerError);
        m_running.store(false, std::memory_order_release);
        return;
    }

    const int packetsPerObject = std::max(1, m_config.targetSegmentBytes / packetizer.packetSize());
    // Unmodified carriage: unmodified-program for a one-program PAT,
    // unmodified-multiplex otherwise. open() fails without a PAT.
    const Mpeg2tsMode mode = !m_config.transparentMode             ? Mpeg2tsMode::PerProgram
                             : packetizer.patProgramCount() == 1   ? Mpeg2tsMode::UnmodifiedProgram
                                                                   : Mpeg2tsMode::UnmodifiedMultiplex;
    const bool multiplex = mode == Mpeg2tsMode::UnmodifiedMultiplex;
    // A multiplex is named mux-1 (draft "Unmodified Multiplex" example). With a
    // reference program the name carries it: a declared track cannot change
    // its program number (draft "Catalog"), so another reference needs another
    // name.
    const QString trackName = !multiplex                ? QStringLiteral("program-1")
                              : packetizer.namesProgram() ? QStringLiteral("mux-1-p%1").arg(packetizer.programNumber())
                                                          : QStringLiteral("mux-1");
    const QString timelineTrackName = trackName + QStringLiteral(".timeline");
    // The skip-ahead warnings name the track, not the FIFO that SRT ingest
    // writes into, as for a multi-program source.
    packetizer.setLabel(trackName);
    // A non-seekable source (FIFO / stdin / pipe) is a live feed: advertise it as
    // live and skip VOD-only duration probing, which would open and consume the
    // pipe a second time.
    const bool liveStream = packetizer.sequential();
    m_config.liveSource = liveStream;
    const qint64 fileDurationMs = liveStream ? 0 : M2tsPacketizer::probeDurationMs(sourcePath);
    MsftsCatalog catalogSpec = packetizerCatalog(packetizer, {
        .track = trackName,
        .packetsPerObject = packetsPerObject,
        .trackDurationMs = fileDurationMs,
        .mode = mode,
        .mpeg2tsMuxRateBps = m_config.mpeg2tsMuxRateBps > 0 ? m_config.mpeg2tsMuxRateBps : packetizer.measuredMuxRate(),
        .isLive = false,
        .bitrateBps = trackMaxBitrate(packetizer, m_config),
        // generatedAt is suppressed for VOD by catalogJson (isLive false).
    });
    if (liveStream) {
        // Live pipe feed: mark the track live (VOD duration does not apply).
        catalogSpec.isLive = true;
        // generatedAt is Optional in MSF 5.1.6 everywhere, so this is a choice rather
        // than a requirement: it is genuinely useful on a live feed for telling one
        // catalog instance from another, and every live example in both drafts carries
        // it. 5.1.6 only says it SHOULD NOT appear when isLive is false, which is why
        // it is set here rather than in the initializer above. The capture path
        // already sets it; this is the path production uses.
        catalogSpec.generatedAtMs = QDateTime::currentMSecsSinceEpoch();
    }
    // Draft "Mux Rate": a per-program track without null packets SHOULD
    // declare mpeg2tsMuxRate. Say why when it cannot.
    if (!m_config.transparentMode && !m_config.retainNullPackets && catalogSpec.mpeg2tsMuxRateBps <= 0) {
        if (packetizer.patProgramCount() > 1) {
            qWarning("No mpeg2tsMuxRate: for a program of a multi-program source, give --mux-rate, at least "
                     "the peak rate of the program.");
        } else {
            qWarning("No mpeg2tsMuxRate: %s. Give --mux-rate to declare one.",
                     qUtf8Printable(packetizer.muxRateNote()));
        }
    }
    if (m_config.transparentMode && m_config.mpeg2tsMuxRateBps > 0) {
        qWarning("--mux-rate is not declared as mpeg2tsMuxRate: MSFTS forbids the field in both "
                 "unmodified modes. It remains a candidate for the catalog bitrate.");
    }
    if (multiplex) {
        if (packetizer.namesProgram()) {
            emit status(QStringLiteral("unmodified-multiplex track %1: reference program %2, PCR PID %3.")
                            .arg(trackName)
                            .arg(packetizer.programNumber())
                            .arg(packetizer.pcrPid()));
        } else {
            emit status(QStringLiteral("unmodified-multiplex track %1: %2 programs, no reference program "
                                       "(give --program to name one).")
                            .arg(trackName)
                            .arg(packetizer.patProgramCount()));
        }
        if (catalogSpec.initData.isEmpty()) {
            qWarning("The catalog carries no initRef: the PMT that initData carries (of the program --program "
                     "names or, without it, of the first program in the PAT) did not arrive at the start, and a "
                     "later catalog cannot add initRef to the track.");
        }
    }
    const QByteArray catalog = MsftsMuxer::catalogJson(catalogSpec);
    if (m_config.resyncPid) {
        m_config.resyncPid->store(packetizer.groupPid(), std::memory_order_relaxed);
    }

    constexpr std::int64_t kPaceSlackUs = 10000; // 10 ms slack    // Shared with the lambda by VALUE, so nothing it touches lives on this frame.
    auto st = std::make_shared<PublishState>();
    st->packetizer = packetizerPtr;
    st->catalog = catalogSpec;

    auto nextObject = [this, st, packetsPerObject, trackName, timelineTrackName, liveStream]() -> std::optional<PublishedObject> {
        if (!m_running.load(std::memory_order_acquire)) {
            return std::nullopt;
        }
        if (!st->pending.empty()) {
            PublishedObject next = std::move(st->pending.front());
            st->pending.pop_front();
            return next;
        }

        M2tsObject object;
        QString readError;
        if (!(*st->packetizer).readObject(packetsPerObject, &object, &readError)) {
            if (!readError.isEmpty()) {
                emit error(readError);
                return std::nullopt;
            }
            // Only a clean end of the source ends the broadcast: not an error,
            // and not an interrupt, which closes the SRT pipe after it clears
            // m_running.
            return m_running.load(std::memory_order_acquire) ? endBroadcast(st.get()) : std::nullopt;
        }
        if (m_config.resyncPid) {
            m_config.resyncPid->store(st->packetizer->groupPid(), std::memory_order_relaxed);
        }

        PublishedObject published;
        published.trackName = trackName;
        published.payload = std::move(object.payload);
        published.groupId = object.groupId;
        published.objectId = object.objectId;
        published.startsGroup = object.startsGroup;
        published.mediaTimeUs = static_cast<std::uint64_t>(st->objects) * static_cast<std::uint64_t>(m_config.fragmentDurationMs) * 1000ULL;
        published.mediaDurationUs = static_cast<std::uint64_t>(m_config.fragmentDurationMs) * 1000ULL;
        // One timeline record per media Group, for its first Object that starts
        // a video PES: MSF 7.1.1 gives the record the PTS of the first media
        // sample in the Object. mediaTimeUs above stays the pacing clock.
        if (object.ptsUs.has_value() && st->timeline.lastGroupId != published.groupId) {
            // MSF 7.1.1: the wallclock time SHOULD be 0 for a VOD asset.
            st->pending.push_back(timelineObject(&st->timeline, timelineTrackName, published,
                                                 *object.ptsUs, liveStream ? nowUnixUs() : 0));
        }
        ++st->objects;
        st->bytes += published.payload.size();
        emit stats(st->objects, st->bytes, static_cast<int64_t>(published.groupId + 1));

        // Pace file-source publishing at media-time rate so data isn't dumped
        // at wire speed before subscribers can connect.
        if (m_config.pacedFileSource) {
            // The release time follows the source's own clock (PCR), so a
            // source is played out at its rate whatever the Object size. An
            // Object without a PCR waits for the media time of the last one.
            const std::int64_t pcrMediaUs = st->pcrClock.update(object.pcr27);
            const bool onPcr = pcrMediaUs >= 0;
            if (onPcr != st->pacingOnPcr) {
                st->pacingOnPcr = onPcr;
                st->pacingStartUs = -1;
            }
            const std::int64_t paceUs = onPcr ? pcrMediaUs : static_cast<std::int64_t>(published.mediaTimeUs);
            if (st->pacingStartUs < 0) {
                st->pacingStartUs = nowSteadyUs() - paceUs;
            }
            while (m_running.load(std::memory_order_acquire)) {
                const std::int64_t delay = paceDelayUs(paceUs, nowSteadyUs() - st->pacingStartUs, kPaceSlackUs);
                if (delay <= 0) {
                    break;
                }
                std::this_thread::sleep_for(std::chrono::microseconds(std::min<std::int64_t>(delay, 5000)));
            }
        }

        // Draft "Use of MSF Initialization Data": the new initData reaches the
        // catalog before the Objects that rely on the changed PSI, this one first.
        QByteArray initData;
        if (st->packetizer->takeInitDataChange(&initData)) {
            st->pending.push_front(std::move(published));
            return updateCatalog(st.get(), initData);
        }
        return published;
    };

    if (!m_publisher->publishLiveObjects(m_config, trackName, QStringList{timelineTrackName}, catalog, std::move(nextObject), m_running)) {
        if (m_running.load(std::memory_order_acquire)) {
            emit error(QStringLiteral("Failed to publish MSFTS live object stream."));
        }
    }

    m_running.store(false, std::memory_order_release);
    emit status(QStringLiteral("Pipeline exiting."));
}

} // namespace moq2ts
