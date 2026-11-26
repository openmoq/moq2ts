#pragma once

#include <QObject>
#include <QString>
#include <QStringList>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <memory>
#include <vector>

#include "MoqxrPublisher.h"
#include "PathQueue.h"

namespace moq2ts {

// Publishes the same Objects to several relays (PublishConfig::moqEndpoints),
// each over its own MoqxrPublisher: a path. The thread that calls
// publishLiveObjects pulls the pipeline's Objects and pushes each one to the
// PathQueue of every path, and each path publishes from its queue on its own
// thread, so a slow or lost relay never holds up the source or the other
// relays. Media and timeline Group and Object IDs come from the pipeline, so
// they are the same on every path; each path numbers its catalog Groups.
//
// A path that fails only logs: the publish fails once every path has failed.
// One relay needs none of this, and the CLI then uses a MoqxrPublisher alone
// (see needed()).
class RedundantPublisher final : public QObject, public IMoqOutput {
    Q_OBJECT
public:
    // Makes the output of path index. Tests replace the MoqxrPublishers with it.
    using PathFactory = std::function<std::unique_ptr<IMoqOutput>(int index)>;

    // How long a path that waits for an Object waits before it checks for a
    // stop request.
    static constexpr std::chrono::milliseconds kPopWait{100};
    // Once the source has ended, a path that has started to publish and then
    // takes no Object for this long (its relay is down, or it waits to connect
    // again) is stopped, so that it cannot hold up the end of the publish.
    static constexpr std::chrono::seconds kDrainTimeout{5};
    // Once the source has ended, a path that has taken no Object yet (it waits
    // for its first subscriber, or is still connecting), or that has taken the
    // end of its queue and has not finished its session (the final catalog or
    // the FIN is not acknowledged), is given this long before it is stopped.
    // One that has taken the end gets PublishConfig::closeDelayMs on top,
    // since its publisher holds the session that long before it closes it.
    static constexpr std::chrono::seconds kIdleTimeout{30};

    explicit RedundantPublisher(QObject* parent = nullptr);
    RedundantPublisher(PathFactory factory, std::int64_t queueBytes, QObject* parent = nullptr);
    ~RedundantPublisher() override;

    // True when cfg names several relays. With one, the MoqxrPublisher works
    // alone, as before.
    static bool needed(const PublishConfig& cfg) { return cfg.moqEndpoints.size() > 1; }

    // Makes one path per relay of cfg.moqEndpoints, on the first call.
    bool connect(const PublishConfig& cfg) override;
    bool publishLiveObjects(const PublishConfig& cfg,
                            const QString& mediaTrackName,
                            const QStringList& sideTrackNames,
                            const QByteArray& catalog,
                            std::function<std::optional<PublishedObject>()> nextObject,
                            std::atomic<bool>& running) override;
    void stop() override;
    bool connected() const override;

    // Replaces the two timeouts above, for a source that ends before its
    // relays are ready, or for tests.
    void setTimeouts(std::chrono::milliseconds drain, std::chrono::milliseconds idle) {
        m_drainTimeout = drain;
        m_idleTimeout = idle;
    }

    int pathCount() const { return static_cast<int>(m_paths.size()); }
    // Whether the relay of path index has accepted the namespace; false for a
    // path whose output is not a MoqxrPublisher.
    bool pathNamespaceAccepted(int path) const;
    // "path=N sent_objects=… sent_bytes=… dropped_objects=… dropped_bytes=…
    // queued_bytes=…" for the periodic line, with what moqxr has sent so far.
    QString pathStatsFields(int path);
    // The fields of the aggregate [stats] and [final] lines, summed over the
    // paths: "sent_objects=… sent_bytes=…" and "pulled_objects=… pulled_bytes=…
    // sent_objects=… sent_bytes=…", in the format of a single relay. "unknown"
    // while no path has a sent count.
    QString statsFields();
    QString finalFields() const;
    // "path=N pulled_objects=… pulled_bytes=… sent_objects=… sent_bytes=…
    // dropped_objects=… dropped_bytes=…" for the line printed at exit.
    QString pathFinalFields(int path) const;

signals:
    // A path's state carries its "path=N" label.
    void connectionStateChanged(bool connected, const QString& message);
    // Only when no path can publish: a path's own errors are logged.
    void publishError(const QString& error);
    // As MoqxrPublisher::groupOpened, once for all the paths: when the first
    // path opens the Group. Emitted from a path's thread.
    void groupOpened(const QString& track, qulonglong groupId);

private:
    std::mutex m_groupMutex;
    std::map<QString, qulonglong> m_openedGroup;
    struct Path {
        explicit Path(std::int64_t queueBytes)
            : queue(queueBytes) {}
        QString endpoint;
        std::unique_ptr<IMoqOutput> output;
        // output, when it is a MoqxrPublisher.
        MoqxrPublisher* moqxr = nullptr;
        PathQueue queue;
        // What the path pulled and what moqxr sent, from its signals.
        PublishTotals totals;
        // The path's publish of the current call has ended, and how.
        std::atomic<bool> done{false};
        std::atomic<bool> ok{false};
    };

    std::unique_ptr<Path> makePath(int index, const QString& endpoint);
    // cfg for path index: its relay only.
    PublishConfig pathConfig(const PublishConfig& cfg, int index) const;
    // Waits until every path has ended, stopping one that holds up the end.
    void waitForPaths();

    PathFactory m_factory;
    std::int64_t m_queueBytes = kPathQueueBytes;
    std::chrono::milliseconds m_drainTimeout{kDrainTimeout};
    std::chrono::milliseconds m_idleTimeout{kIdleTimeout};
    // PublishConfig::closeDelayMs of the current publish.
    std::chrono::milliseconds m_closeDelay{0};
    // The endpoints the paths were made for.
    QStringList m_endpoints;
    // Made by the first connect(), on the thread that owns this object. A
    // later connect() must name the same endpoints.
    std::vector<std::unique_ptr<Path>> m_paths;
    std::atomic<bool> m_connected{false};
};

} // namespace moq2ts
