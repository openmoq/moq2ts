#include "RedundantPublisher.h"

#include <QDebug>
#include <QThread>

#include <algorithm>
#include <thread>

namespace moq2ts {

RedundantPublisher::RedundantPublisher(QObject* parent)
    : RedundantPublisher([](int) { return std::make_unique<MoqxrPublisher>(); }, kPathQueueBytes, parent) {}

RedundantPublisher::RedundantPublisher(PathFactory factory, std::int64_t queueBytes, QObject* parent)
    : QObject(parent),
      m_factory(std::move(factory)),
      m_queueBytes(queueBytes) {}

RedundantPublisher::~RedundantPublisher() {
    stop();
}

std::unique_ptr<RedundantPublisher::Path> RedundantPublisher::makePath(int index, const QString& endpoint) {
    auto path = std::make_unique<Path>(m_queueBytes);
    path->endpoint = endpoint;
    path->output = m_factory(index);
    path->moqxr = dynamic_cast<MoqxrPublisher*>(path->output.get());
    if (path->moqxr != nullptr) {
        Path* raw = path.get();
        // Emitted from the path's thread, hence direct.
        QObject::connect(path->moqxr, &MoqxrPublisher::framePublished, this,
                         [raw](const QString&, int64_t bytes, int64_t objects) {
                             raw->totals.recordPulled(objects, bytes);
                         },
                         Qt::DirectConnection);
        QObject::connect(path->moqxr, &MoqxrPublisher::groupOpened, this,
                         [this](const QString& track, qulonglong group) {
                             std::lock_guard lock(m_groupMutex);
                             const auto [opened, first] = m_openedGroup.try_emplace(track, group);
                             if (first || group > opened->second) {
                                 opened->second = group;
                                 emit groupOpened(track, group);
                             }
                         },
                         Qt::DirectConnection);
        QObject::connect(path->moqxr, &MoqxrPublisher::sentTotals, this,
                         [raw](int64_t objects, int64_t bytes) { raw->totals.recordSent(objects, bytes); },
                         Qt::DirectConnection);
        // One path that fails does not fail the publish: its error is logged.
        QObject::connect(path->moqxr, &MoqxrPublisher::publishError, this,
                         [index](const QString& message) {
                             qWarning("path=%d: %s", index, qUtf8Printable(message));
                         },
                         Qt::DirectConnection);
        QObject::connect(path->moqxr, &MoqxrPublisher::connectionStateChanged, this,
                         [this, index](bool connected, const QString& message) {
                             emit connectionStateChanged(connected, QStringLiteral("path=%1: %2").arg(index).arg(message));
                         },
                         Qt::DirectConnection);
        // A new connection after a lost one goes on at the next Group, from
        // what the queue receives then: what queued up meanwhile is stale.
        path->moqxr->setConnectionStartHook([raw](int connection) -> std::optional<QByteArray> {
            if (connection == 0) {
                return std::nullopt;
            }
            return raw->queue.flushForResume();
        });
    }
    return path;
}

PublishConfig RedundantPublisher::pathConfig(const PublishConfig& cfg, int index) const {
    PublishConfig path = cfg;
    path.moqEndpoint = m_paths.at(static_cast<std::size_t>(index))->endpoint;
    path.moqEndpoints.clear();
    return path;
}

bool RedundantPublisher::connect(const PublishConfig& cfg) {
    if (m_paths.empty()) {
        for (int index = 0; index < cfg.moqEndpoints.size(); ++index) {
            m_paths.push_back(makePath(index, cfg.moqEndpoints.at(index)));
        }
        m_endpoints = cfg.moqEndpoints;
    } else if (cfg.moqEndpoints != m_endpoints) {
        // The paths and their queues belong to the first call's relays.
        emit publishError(QStringLiteral("The relays of a publisher cannot change: it was made for %1.")
                              .arg(m_endpoints.join(QStringLiteral(", "))));
        return false;
    }
    if (m_paths.size() < 2) {
        emit publishError(QStringLiteral("Publishing to several relays needs two endpoints or more."));
        return false;
    }
    // connect() only checks the settings, so a path that refuses them is an
    // error of the whole publish.
    for (int index = 0; index < pathCount(); ++index) {
        Path& path = *m_paths[static_cast<std::size_t>(index)];
        if (!path.output->connect(pathConfig(cfg, index))) {
            emit publishError(QStringLiteral("path=%1: cannot open the publish session to %2.").arg(index).arg(path.endpoint));
            for (const auto& other : m_paths) {
                other->output->stop();
            }
            return false;
        }
    }
    m_connected.store(true, std::memory_order_release);
    QStringList endpoints;
    for (const auto& path : m_paths) {
        endpoints << path->endpoint;
    }
    emit connectionStateChanged(true, QStringLiteral("Publishing the same Objects to %1 relays: %2")
                                          .arg(pathCount())
                                          .arg(endpoints.join(QStringLiteral(", "))));
    return true;
}

bool RedundantPublisher::publishLiveObjects(const PublishConfig& cfg,
                                            const QString& mediaTrackName,
                                            const QStringList& sideTrackNames,
                                            const QByteArray& catalog,
                                            std::function<std::optional<PublishedObject>()> nextObject,
                                            std::atomic<bool>& running) {
    if (!m_connected.load(std::memory_order_acquire)) {
        emit publishError(QStringLiteral("Cannot publish: no active relay session."));
        return false;
    }

    std::vector<std::thread> threads;
    for (int index = 0; index < pathCount(); ++index) {
        Path& path = *m_paths[static_cast<std::size_t>(index)];
        const PublishConfig pathCfg = pathConfig(cfg, index);
        path.queue.reopen();
        path.ok.store(false, std::memory_order_release);
        path.done.store(false, std::memory_order_release);
        // A path that the end of an earlier session stopped (a PAT change
        // starts a new one) connects again.
        if (!path.output->connected() && !path.output->connect(pathCfg)) {
            qWarning("path=%d: cannot open the publish session to %s.", index, qUtf8Printable(path.endpoint));
            path.queue.close(true);
            path.done.store(true, std::memory_order_release);
            continue;
        }
        threads.emplace_back([this, &path, index, pathCfg, &mediaTrackName, &sideTrackNames, &catalog, &running]() {
            PathQueue* queue = &path.queue;
            auto next = [this, queue, &running]() -> std::optional<PublishedObject> {
                if (!running.load(std::memory_order_acquire) || !m_connected.load(std::memory_order_acquire)) {
                    return std::nullopt;
                }
                if (std::optional<PublishedObject> object = queue->pop(kPopWait)) {
                    return object;
                }
                if (queue->drained()) {
                    return std::nullopt;
                }
                PublishedObject nothingYet;
                nothingYet.idle = true;
                return nothingYet;
            };
            const bool ok = path.output->publishLiveObjects(pathCfg, mediaTrackName, sideTrackNames, catalog,
                                                            std::move(next), running);
            path.ok.store(ok, std::memory_order_release);
            // Nobody takes from the queue of a path that has ended.
            path.queue.close(true);
            path.done.store(true, std::memory_order_release);
            if (!ok) {
                qWarning("path=%d: the publish to %s failed; the other relays carry on.", index,
                         qUtf8Printable(path.endpoint));
            }
        });
    }

    // The producer: every Object goes to the queue of every path, which
    // shares its payload. It stops once no path publishes.
    const auto anyPathLeft = [this] {
        for (const auto& path : m_paths) {
            if (!path->done.load(std::memory_order_acquire)) {
                return true;
            }
        }
        return false;
    };
    while (m_connected.load(std::memory_order_acquire) && anyPathLeft()) {
        std::optional<PublishedObject> object = nextObject();
        if (!object.has_value()) {
            break;
        }
        // Each path says so itself when its own queue is empty.
        if (object->idle) {
            continue;
        }
        for (const auto& path : m_paths) {
            path->queue.push(*object);
        }
    }
    for (const auto& path : m_paths) {
        path->queue.close();
    }
    waitForPaths();
    for (std::thread& thread : threads) {
        thread.join();
    }

    bool anyOk = false;
    for (const auto& path : m_paths) {
        anyOk = anyOk || path->ok.load(std::memory_order_acquire);
    }
    if (!anyOk && m_connected.load(std::memory_order_acquire)) {
        emit publishError(QStringLiteral("The publish failed on every relay."));
    }
    return anyOk;
}

void RedundantPublisher::waitForPaths() {
    const std::size_t count = m_paths.size();
    const auto start = std::chrono::steady_clock::now();
    std::vector<std::int64_t> popped(count, 0);
    std::vector<std::chrono::steady_clock::time_point> progress(count, start);
    std::vector<bool> endSeen(count, false);
    std::vector<bool> stopped(count, false);
    for (;;) {
        bool waiting = false;
        const auto now = std::chrono::steady_clock::now();
        for (std::size_t index = 0; index < count; ++index) {
            Path& path = *m_paths[index];
            if (path.done.load(std::memory_order_acquire)) {
                continue;
            }
            waiting = true;
            // stop() stops every path.
            if (!m_connected.load(std::memory_order_acquire) || stopped[index]) {
                continue;
            }
            const PathQueue::Counters counters = path.queue.counters();
            if (counters.popped != popped[index]) {
                popped[index] = counters.popped;
                progress[index] = now;
            }
            if (counters.endTaken && !endSeen[index]) {
                endSeen[index] = true;  // the clock for the end of its session starts now
                progress[index] = now;
            }
            // A path that is publishing and stalls is stopped soon. One that
            // has taken nothing yet, or has taken the end and not finished,
            // may only be slow (no subscriber yet, a session that ends), so it
            // gets longer.
            const bool slowToStart = counters.popped == 0 || endSeen[index];
            const std::chrono::milliseconds limit = slowToStart ? m_idleTimeout : m_drainTimeout;
            if (now - progress[index] >= limit) {
                qWarning("path=%d: %s %s for %lld ms after the source ended (%lld Objects left); stopping it.",
                         static_cast<int>(index), qUtf8Printable(path.endpoint),
                         endSeen[index] ? "did not finish its session"
                                        : (counters.popped == 0 ? "took no Object" : "took no further Object"),
                         static_cast<long long>(limit.count()), static_cast<long long>(counters.queuedObjects));
                stopped[index] = true;
                path.output->stop();
            }
        }
        if (!waiting) {
            return;
        }
        QThread::msleep(20);
    }
}

void RedundantPublisher::stop() {
    if (!m_connected.exchange(false, std::memory_order_acq_rel)) {
        return;
    }
    for (const auto& path : m_paths) {
        path->output->stop();
        path->queue.close();
    }
    emit connectionStateChanged(false, QStringLiteral("Publisher stopped"));
}

bool RedundantPublisher::connected() const {
    return m_connected.load(std::memory_order_acquire);
}

bool RedundantPublisher::pathNamespaceAccepted(int index) const {
    const Path& path = *m_paths.at(static_cast<std::size_t>(index));
    return path.moqxr != nullptr && path.moqxr->namespaceAccepted();
}

QString RedundantPublisher::statsFields() {
    int64_t objects = -1;
    int64_t bytes = -1;
    for (auto& path : m_paths) {
        int64_t pathObjects = 0;
        int64_t pathBytes = 0;
        if (path->moqxr != nullptr && path->moqxr->sentSoFar(pathObjects, pathBytes)) {
            path->totals.observeSent(pathObjects, pathBytes);
        }
        pathObjects = path->totals.observedObjects();
        pathBytes = path->totals.observedBytes();
        if (pathObjects >= 0) {
            objects = std::max<int64_t>(objects, 0) + pathObjects;
            bytes = std::max<int64_t>(bytes, 0) + pathBytes;
        }
    }
    const auto text = [](int64_t value) { return value < 0 ? QStringLiteral("unknown") : QString::number(value); };
    return QStringLiteral("sent_objects=%1 sent_bytes=%2").arg(text(objects), text(bytes));
}

QString RedundantPublisher::finalFields() const {
    PublishTotals sum;
    int64_t pulledObjects = 0;
    int64_t pulledBytes = 0;
    int64_t sentObjects = -1;
    int64_t sentBytes = -1;
    for (const auto& path : m_paths) {
        pulledObjects += path->totals.pulledObjects();
        pulledBytes += path->totals.pulledBytes();
        const int64_t objects = path->totals.finalSentObjects();
        if (objects >= 0) {
            sentObjects = std::max<int64_t>(sentObjects, 0) + objects;
            sentBytes = std::max<int64_t>(sentBytes, 0) + path->totals.finalSentBytes();
        }
    }
    sum.recordPulled(pulledObjects, pulledBytes);
    if (sentObjects >= 0) {
        sum.recordSent(sentObjects, sentBytes);
    }
    return sum.finalFields();
}

QString RedundantPublisher::pathStatsFields(int index) {
    Path& path = *m_paths.at(static_cast<std::size_t>(index));
    int64_t objects = 0;
    int64_t bytes = 0;
    if (path.moqxr != nullptr && path.moqxr->sentSoFar(objects, bytes)) {
        path.totals.observeSent(objects, bytes);
    }
    const PathQueue::Counters counters = path.queue.counters();
    return QStringLiteral("path=%1 %2 dropped_objects=%3 dropped_bytes=%4 queued_bytes=%5")
        .arg(index)
        .arg(path.totals.sentFields())
        .arg(counters.droppedObjects)
        .arg(counters.droppedBytes)
        .arg(counters.queuedBytes);
}

QString RedundantPublisher::pathFinalFields(int index) const {
    const Path& path = *m_paths.at(static_cast<std::size_t>(index));
    const PathQueue::Counters counters = path.queue.counters();
    return QStringLiteral("path=%1 %2 dropped_objects=%3 dropped_bytes=%4")
        .arg(index)
        .arg(path.totals.finalFields())
        .arg(counters.droppedObjects)
        .arg(counters.droppedBytes);
}

} // namespace moq2ts
