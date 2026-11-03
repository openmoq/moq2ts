#include "MptsPipeline.h"

#include <QDateTime>
#include <QDebug>
#include <QFile>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <optional>
#include <utility>
#include <vector>

#include "../publish/MoqxrPublisher.h"
#include "LivePipeline.h"
#include "M2tsPacketizer.h"
#include "MptsFanOut.h"
#include "MsftsMuxer.h"
#include "PacketQueueDevice.h"
#include "SourceGapLog.h"

namespace moq2ts {

namespace {

// The merged queue holds this many Objects per configured program, as one
// pool that every program shares: a program may hold more than its share
// while the others hold fewer. Few, so that a publisher that does not pull
// (no subscriber yet) holds the workers back at once, and the program queues,
// which drop their oldest packets, take the wait.
constexpr std::size_t kMergedQueueSlotsPerProgram = 4;

// One configured program: its queue from the fan-out, its packetizer, and the
// timeline history of its track.
struct Program {
    MptsProgramConfig config;
    std::shared_ptr<PacketQueueDevice> device;
    std::shared_ptr<M2tsPacketizer> packetizer;
    QString track;
    QString timelineTrack;
    int packetsPerObject = 1;
    std::int64_t objects = 0;
    TimelineHistory timeline;
};

} // namespace

// What the workers and the publisher share: the merged queue and the catalog.
// The catalog is updated under the queue lock, so catalog Groups go out in the
// order of their numbers.
struct MptsPipeline::Shared {
    std::mutex mutex;
    std::condition_variable notFull;
    std::condition_variable notEmpty;
    std::deque<PublishedObject> queue;
    std::size_t capacity = 0;   // Objects, all programs together
    int liveWorkers = 0;
    bool stopping = false;
    QList<MsftsCatalog> catalogs;   // one per program, in configuration order
    QByteArray catalogJson;
    std::uint64_t catalogGroupId = 0;
    // The queue of each program, and from the first pull on, the offset in
    // its stream where live media starts: an Object of the program that
    // starts before it was read before the first pull, and is dropped.
    std::vector<std::shared_ptr<PacketQueueDevice>> devices;
    std::vector<std::uint64_t> liveFrom;
    // Per program: an Object went out since the first pull.
    std::vector<bool> liveStarted;
    bool pulled = false;
    // A program ended on an error or an interrupt, not at the end of its
    // source; and the closing catalog has been handed out.
    bool unclean = false;
    bool broadcastEnded = false;

    enum class Pushed { Stopped, Dropped, Queued, QueuedFirstLive };

    // Queues the Objects of a program as one run, after a new catalog when
    // initData is given. mediaOffset is M2tsObject::sourceOffset of the media
    // Object that objects are or describe, if any: Dropped when that Object
    // is stale, and QueuedFirstLive for the first media Object of the program
    // queued after the first pull. Waits for room; Stopped once stopping.
    Pushed push(int program, std::vector<PublishedObject> objects, const QByteArray* initData,
                std::optional<std::uint64_t> mediaOffset, bool media) {
        std::unique_lock<std::mutex> lock(mutex);
        // Media read before the first pull is stale, but the catalog it
        // changed still goes out. The pull can come during the wait.
        const auto stale = [&] { return mediaOffset.has_value() && *mediaOffset < liveFrom[program]; };
        const auto needed = [&] { return (stale() ? 0 : objects.size()) + (initData ? 1 : 0); };
        notFull.wait(lock, [&] { return stopping || queue.size() + needed() <= capacity; });
        if (stopping) {
            return Pushed::Stopped;
        }
        Pushed pushed = Pushed::Queued;
        if (stale()) {
            objects.clear();
            pushed = Pushed::Dropped;
        } else if (media && pulled && !liveStarted[program]) {
            liveStarted[program] = true;
            pushed = Pushed::QueuedFirstLive;
        }
        if (initData) {
            queue.push_back(updateCatalog(program, *initData));
        }
        for (PublishedObject& object : objects) {
            queue.push_back(std::move(object));
        }
        notEmpty.notify_one();
        return pushed;
    }

    // The next Object, or none once every program ended or on stop. When
    // every program ended at the end of its source, the last Object is the
    // closing catalog (MSF -01 Section 11.3).
    std::optional<PublishedObject> pop() {
        std::unique_lock<std::mutex> lock(mutex);
        notEmpty.wait(lock, [&] { return stopping || !queue.empty() || liveWorkers == 0; });
        if (stopping) {
            return std::nullopt;
        }
        if (queue.empty()) {
            return endBroadcast();
        }
        PublishedObject next = std::move(queue.front());
        queue.pop_front();
        notFull.notify_all();
        return next;
    }

    // The publisher's first pull, once a subscriber came: nothing read before
    // it goes out. The media Objects and their timeline records queued so far
    // go, but not the catalogs and the track ends. Each program queue drops
    // what it holds and marks a gap, so each program's next Group starts at
    // its next random access point after it, with the PCR discontinuity
    // marked. Once only; before the first pop().
    void startAtLiveEdge() {
        std::lock_guard<std::mutex> lock(mutex);
        if (pulled) {
            return;
        }
        pulled = true;
        std::deque<PublishedObject> kept;
        for (PublishedObject& object : queue) {
            if (object.trackName == QLatin1String(kCatalogTrackName) || object.endsTrack) {
                kept.push_back(std::move(object));
            }
        }
        queue = std::move(kept);
        for (std::size_t index = 0; index < devices.size(); ++index) {
            liveFrom[index] = devices[index]->flushToLiveEdge();
        }
        notFull.notify_all();
    }

    // The independent catalog that ends the broadcast for good: no track and
    // isComplete. Sent once, and only when no program ended on an error or an
    // interrupt, which commit to nothing about the content that follows.
    // moqxr ends every other track before it sends this one.
    std::optional<PublishedObject> endBroadcast() {
        if (unclean || broadcastEnded || catalogs.isEmpty() || !catalogs.first().isLive) {
            return std::nullopt;
        }
        broadcastEnded = true;
        MsftsCatalog closing = catalogs.first();
        closing.generatedAtMs = QDateTime::currentMSecsSinceEpoch();
        PublishedObject catalog;
        catalog.trackName = QLatin1String(kCatalogTrackName);
        catalog.payload = MsftsMuxer::completeCatalogJson(closing);
        catalog.groupId = ++catalogGroupId;
        catalog.objectId = 0;
        catalog.startsGroup = true;
        catalog.finalInGroup = true;
        catalog.endsBroadcast = true;
        return catalog;
    }

    void workerEnded(bool clean) {
        std::lock_guard<std::mutex> lock(mutex);
        unclean = unclean || !clean;
        --liveWorkers;
        notEmpty.notify_all();
    }

    void stop() {
        std::lock_guard<std::mutex> lock(mutex);
        stopping = true;
        notFull.notify_all();
        notEmpty.notify_all();
    }

    // A PSI change of one program: a new independent catalog that lists every
    // track, in the next catalog Group, as MSFTS 6.12 asks (as LivePipeline's
    // updateCatalog).
    PublishedObject updateCatalog(int program, const QByteArray& initData) {
        catalogs[program].initData = initData;
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        for (MsftsCatalog& catalog : catalogs) {
            catalog.generatedAtMs = now;
        }
        catalogJson = MsftsMuxer::catalogJson(catalogs);
        PublishedObject catalog;
        catalog.trackName = QLatin1String(kCatalogTrackName);
        catalog.payload = catalogJson;
        catalog.groupId = ++catalogGroupId;
        catalog.objectId = 0;
        catalog.startsGroup = true;
        catalog.finalInGroup = true;
        return catalog;
    }
};

MptsPipeline::MptsPipeline(QObject* parent)
    : QObject(parent) {}

MptsPipeline::~MptsPipeline() {
    stop();
}

bool MptsPipeline::running() const {
    return m_running.load(std::memory_order_acquire);
}

QByteArray MptsPipeline::catalogJson() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_shared) {
        std::lock_guard<std::mutex> sharedLock(m_shared->mutex);
        return m_shared->catalogJson;
    }
    return m_catalogJson;
}

void MptsPipeline::start(const PublishConfig& relay, const MptsConfig& config, IMoqOutput* publisher) {
    if (m_running.load(std::memory_order_acquire)) {
        emit status(QStringLiteral("Pipeline already running."));
        return;
    }
    if (!publisher) {
        emit error(QStringLiteral("No publisher instance provided."));
        return;
    }
    if (config.programs.isEmpty()) {
        emit error(QStringLiteral("No program to publish."));
        return;
    }
    if (m_thread.joinable()) {
        m_thread.join();   // the previous run, which has ended
    }
    m_relay = relay;
    // A FIFO, a pipe, or SRT: run() refuses a seekable file.
    m_relay.liveSource = true;
    m_config = config;
    m_publisher = publisher;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_catalogJson.clear();
    }
    m_running.store(true, std::memory_order_release);
    // Set before the thread starts, so that a stop() that comes first still
    // ends the open() of a FIFO, which only blocks for a FIFO.
    struct stat info {};
    m_openingSource.store(::stat(QFile::encodeName(config.sourcePath).constData(), &info) == 0 && S_ISFIFO(info.st_mode),
                          std::memory_order_release);
    m_thread = std::thread([this] { run(); });
}

void MptsPipeline::stop() {
    m_running.store(false, std::memory_order_release);
    // Taken under the lock and stopped outside it: stopping the fan-out joins
    // its thread, which must not hold back catalogJson().
    std::shared_ptr<Shared> shared;
    std::shared_ptr<MptsFanOut> fanOut;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        shared = m_shared;
        fanOut = m_fanOut;
    }
    if (shared) {
        shared->stop();
    }
    // Ends a wait for the first packets and aborts the program queues,
    // which wakes the packetizers blocked on them.
    if (fanOut) {
        fanOut->stop();
    }
    // Opening a FIFO for reading blocks until a writer opens it, and nothing
    // interrupts open(2) from another thread: open it for writing, which ends
    // that wait. The reader then sees the end of the source.
    const QByteArray path = QFile::encodeName(m_config.sourcePath);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (m_openingSource.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline) {
        const int fd = ::open(path.constData(), O_WRONLY | O_NONBLOCK);
        if (fd >= 0) {
            ::close(fd);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (m_thread.joinable()) {
        m_thread.join();
    }
}

void MptsPipeline::run() {
    // Errors go out before running() turns false, so a caller that sees the
    // pipeline stopped has its reason.
    const auto fail = [this](const QString& message) {
        if (m_running.load(std::memory_order_acquire)) {
            emit error(message);
        }
        m_running.store(false, std::memory_order_release);
    };

    auto source = std::make_shared<QFile>(m_config.sourcePath);
    const bool opened = source->open(QIODevice::ReadOnly);
    m_openingSource.store(false, std::memory_order_release);
    if (!opened) {
        fail(QStringLiteral("Failed to open the source %1: %2").arg(m_config.sourcePath, source->errorString()));
        return;
    }
    // Every track is declared live, and the fan-out serves a stream that it
    // reads once, so a seekable file is refused.
    if (!source->isSequential()) {
        fail(QStringLiteral("multi-program publishing needs a live source (FIFO, pipe, or SRT)"));
        return;
    }

    auto fanOut = std::make_shared<MptsFanOut>(
        source, m_config.sourceGaps ? m_config.sourceGaps : std::make_shared<SourceGapLog>());
    std::vector<std::unique_ptr<Program>> programs;
    for (const MptsProgramConfig& config : m_config.programs) {
        auto program = std::make_unique<Program>();
        program->config = config;
        program->device = fanOut->addOutput(kMptsOutputCapacityBytes);
        program->track = QStringLiteral("program-%1").arg(config.programNumber);
        program->timelineTrack = program->track + QStringLiteral(".timeline");
        programs.push_back(std::move(program));
    }
    auto shared = std::make_shared<Shared>();
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_running.load(std::memory_order_acquire)) {
            return;
        }
        m_fanOut = fanOut;
        m_shared = shared;
    }
    // Every exit from here stops the source and the workers, then keeps the
    // catalog for catalogJson().
    std::vector<std::thread> workers;
    struct Cleanup {
        MptsPipeline* self;
        std::shared_ptr<MptsFanOut> fanOut;
        std::shared_ptr<Shared> shared;
        std::vector<std::thread>* workers;
        ~Cleanup() {
            shared->stop();
            fanOut->stop();   // joins its thread: outside the lock, as in stop()
            {
                std::lock_guard<std::mutex> lock(self->m_mutex);
                self->m_fanOut.reset();
                self->m_shared.reset();
                std::lock_guard<std::mutex> sharedLock(shared->mutex);
                self->m_catalogJson = shared->catalogJson;
            }
            for (std::thread& worker : *workers) {
                worker.join();
            }
            self->m_running.store(false, std::memory_order_release);
        }
    } cleanup{this, fanOut, shared, &workers};

    QString startError;
    if (!fanOut->start(&startError)) {
        fail(startError);
        return;
    }

    // Every program opens at once, each on a thread of its own: an open reads
    // up to 10 s of the source for a random access point, and one after the
    // other, they would add up while the program queues not yet read drop
    // their packets. stop() aborts the queues, which ends every open.
    std::vector<QString> openErrors(programs.size());
    // char, not bool: the threads write their own elements at once.
    std::vector<char> programOpened(programs.size(), 0);
    {
        std::vector<std::thread> openers;
        for (std::size_t index = 0; index < programs.size(); ++index) {
            Program* program = programs[index].get();
            auto packetizer = std::make_shared<M2tsPacketizer>(program->device, program->track);
            // Draft "Per-Program": the program's own SI selection, none when empty.
            packetizer->setSiTables(program->config.siTables);
            packetizer->setSourceGaps(program->device->gaps());
            // It reads the whole multiplex, so its start-up windows are PCR time.
            packetizer->setTimedStartWindows(true);
            program->packetizer = packetizer;
            openers.emplace_back([program, &openErrors, &programOpened, index] {
                programOpened[index] = program->packetizer->open(program->config.programNumber, &openErrors[index]);
            });
        }
        for (std::thread& opener : openers) {
            opener.join();
        }
    }
    for (std::size_t index = 0; index < programs.size(); ++index) {
        Program* program = programs[index].get();
        const M2tsPacketizer& packetizer = *program->packetizer;
        if (!programOpened[index]) {
            // A PAT that was read and does not list the program.
            if (packetizer.patProgramCount() > 0 && packetizer.pmtPid() < 0) {
                fail(QStringLiteral("program %1 is not in the source PAT").arg(program->config.programNumber));
            } else {
                fail(QStringLiteral("program %1: %2").arg(program->config.programNumber).arg(openErrors[index]));
            }
            return;
        }
        program->packetsPerObject = std::max(1, m_config.targetSegmentBytes / packetizer.packetSize());
    }

    // One catalog entry per program, as LivePipeline builds a live per-program
    // track.
    const qint64 generatedAtMs = QDateTime::currentMSecsSinceEpoch();
    QList<MsftsCatalog> catalogs;
    for (const std::unique_ptr<Program>& program : programs) {
        const M2tsPacketizer& packetizer = *program->packetizer;
        // MSFTS 6.7: the configured rate, or one worked out above the rate
        // measured at start-up.
        const qint64 muxRate = program->config.muxRateBps > 0 ? program->config.muxRateBps
                                                               : workedOutMuxRate(packetizer.measuredBitrate());
        const MsftsCatalog entry = packetizerCatalog(packetizer, {
            .track = program->track,
            .packetsPerObject = program->packetsPerObject,
            .mode = Mpeg2tsMode::PerProgram,
            .mpeg2tsMuxRateBps = muxRate,
            .isLive = true,
            // MSF bitrate (5.2.22) is a maximum: the program's mux rate bounds
            // the track. It is 0 only when no rate was measured, and then
            // there is no bitrate to declare either.
            .bitrateBps = muxRate,
            .generatedAtMs = generatedAtMs,
        });
        // MSFTS 6.7: the rate SHOULD NOT be lower than the program's peak
        // rate; a receiver that pads to a lower one overflows. The average
        // measured at start-up is a lower bound of that peak.
        if (program->config.muxRateBps > 0 && packetizer.measuredBitrate() > program->config.muxRateBps) {
            qWarning("The mux rate of program %d, %lld bit/s, is below the rate measured at start-up, %lld bit/s: "
                     "a receiver that pads the track to it overflows (MSFTS 6.7).",
                     program->config.programNumber, static_cast<long long>(program->config.muxRateBps),
                     static_cast<long long>(packetizer.measuredBitrate()));
        }
        if (muxRate <= 0) {
            qWarning("No mpeg2tsMuxRate and no bitrate for program %d: its rate could not be measured from the "
                     "PCR. Give the program a mux rate to declare both.",
                     program->config.programNumber);
        }
        catalogs.append(entry);
        emit status(QStringLiteral("per-program track %1: PCR PID %2, mpeg2tsMuxRate %3 bit/s.")
                        .arg(program->track)
                        .arg(packetizer.pcrPid())
                        .arg(muxRate));
    }
    const QByteArray catalog = MsftsMuxer::catalogJson(catalogs);
    {
        std::lock_guard<std::mutex> lock(shared->mutex);
        shared->catalogs = catalogs;
        shared->catalogJson = catalog;
        shared->liveWorkers = static_cast<int>(programs.size());
        shared->capacity = kMergedQueueSlotsPerProgram * programs.size();
        for (const std::unique_ptr<Program>& program : programs) {
            shared->devices.push_back(program->device);
        }
        shared->liveFrom.assign(programs.size(), 0);
        shared->liveStarted.assign(programs.size(), false);
    }

    const std::uint64_t fragmentUs = static_cast<std::uint64_t>(m_relay.fragmentDurationMs) * 1000ULL;
    for (int index = 0; index < static_cast<int>(programs.size()); ++index) {
        workers.emplace_back([this, shared, program = programs[index].get(), index, fragmentUs] {
            QString readError;
            while (true) {
                M2tsObject object;
                if (!program->packetizer->readObject(program->packetsPerObject, &object, &readError)) {
                    break;
                }
                PublishedObject published;
                published.trackName = program->track;
                published.payload = std::move(object.payload);
                published.groupId = object.groupId;
                published.objectId = object.objectId;
                published.startsGroup = object.startsGroup;
                published.mediaTimeUs = static_cast<std::uint64_t>(program->objects) * fragmentUs;
                published.mediaDurationUs = fragmentUs;
                ++program->objects;
                // Draft "Use of MSF Initialization Data": the new initData
                // reaches the catalog before the Objects that rely on the
                // changed PSI, this one first.
                QByteArray initData;
                const bool initDataChanged = program->packetizer->takeInitDataChange(&initData);
                const Shared::Pushed pushed =
                    shared->push(index, {published}, initDataChanged ? &initData : nullptr, object.sourceOffset, true);
                if (pushed == Shared::Pushed::Stopped) {
                    break;
                }
                if (pushed == Shared::Pushed::Dropped) {
                    continue;
                }
                // The timeline history records the Groups that went out: none
                // from before the first pull.
                if (pushed == Shared::Pushed::QueuedFirstLive) {
                    program->timeline = TimelineHistory();
                }
                // One timeline record per media Group, for its first Object
                // that starts a video PES, with that PES's PTS (MSF 7.1.1). It
                // follows the media Object it records, and goes with it when
                // the first pull comes in between and drops it.
                if (object.ptsUs.has_value() && program->timeline.lastGroupId != published.groupId &&
                    shared->push(index, {timelineObject(&program->timeline, program->timelineTrack, published,
                                                        *object.ptsUs, nowUnixUs())},
                                 nullptr, object.sourceOffset, false) == Shared::Pushed::Stopped) {
                    break;
                }
            }
            // The track has no more Objects: its last one closes its Group
            // now, rather than when the session ends. Timeline Objects are
            // each final in their Group, so that track needs no marker. The
            // marker is queued before programEnded() reports why the track ended.
            PublishedObject end;
            end.trackName = program->track;
            end.endsTrack = true;
            shared->push(index, {end}, nullptr, std::nullopt, false);
            const bool running = m_running.load(std::memory_order_acquire);
            if (!readError.isEmpty() && running) {
                emit programEnded(program->config.programNumber, readError);
            }
            // Only a clean end of the source: not an error, and not an
            // interrupt, which closes the SRT pipe after it clears m_running.
            shared->workerEnded(readError.isEmpty() && running);
        });
    }
    emit status(QStringLiteral("MSFTS multi-program pipeline started: %1 programs.").arg(programs.size()));

    // The publisher's end-of-Group look-ahead works per track name, so the
    // tracks of every program can share one stream of Objects.
    QStringList sideTracks;
    for (const std::unique_ptr<Program>& program : programs) {
        if (program != programs.front()) {
            sideTracks.append(program->track);
        }
        sideTracks.append(program->timelineTrack);
    }
    // The first pull starts every track at the live edge, then says so.
    auto nextObject = [this, shared, first = true]() mutable {
        if (std::exchange(first, false)) {
            shared->startAtLiveEdge();
            emit status(QStringLiteral("First pull: every program starts at its next random access point."));
            emit atLiveEdge();
        }
        return shared->pop();
    };
    if (!m_publisher->publishLiveObjects(m_relay, programs.front()->track, sideTracks, catalog, std::move(nextObject),
                                         m_running)) {
        fail(QStringLiteral("Failed to publish the MSFTS multi-program stream."));
    }
    emit status(QStringLiteral("Pipeline exiting."));
}

} // namespace moq2ts
