#pragma once

#include <QByteArray>
#include <QMutex>
#include <QObject>
#include <QStringList>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>

#include "../app/PublishConfig.h"

#ifdef MOQ2TS_HAS_MOQXR
namespace openmoq::publisher {
class Publisher;
}
#endif

namespace moq2ts {

// The Track Name of the catalog track: MSF -01 Section 5 makes it "catalog".
inline constexpr char kCatalogTrackName[] = "catalog";

struct PublishedObject {
    QString trackName;
    QByteArray payload;
    std::uint64_t groupId = 0;
    std::uint64_t objectId = 0;
    std::uint64_t mediaTimeUs = 0;
    std::uint64_t mediaDurationUs = 0;
    // True when this object contains a random-access point and therefore opens a
    // new group. Carried explicitly rather than derived from objectId == 0: the
    // very first object of a stream has objectId 0 without any RAP
    // (M2tsPacketizer.cpp:465 only bumps the group once sawFirstRap), so deriving
    // it would mislabel exactly one object per run.
    bool startsGroup = false;
    // True when no later object shares this group (a timeline or catalog
    // object), so the publisher closes the group without looking ahead.
    bool finalInGroup = false;
    // A marker, never sent: the track named trackName has no more Objects, so
    // its last Object closes its Group (see ObjectLookahead).
    bool endsTrack = false;
    // The track this Object describes, which must go out first: a timeline
    // Object records a media Object (MSF -01 Section 7: data about previously
    // published Groups). Empty when it depends on none.
    QString afterTrack;
    // The closing catalog of MSF -01 Section 11.3, the source's last object:
    // moqxr ends every other track (PUBLISH_DONE) before sending it.
    bool endsBroadcast = false;
};

// The totals of a publish, for the CLI's [final] line: what the publisher
// pulled from the pipeline and handed to moqxr (framePublished), and what
// moqxr wrote (sentTotals). They differ while nobody subscribes, as moqxr
// drops what it pulls. The sent counts are unknown when moqxr gave none, as
// after a failed publish. Thread-safe.
class PublishTotals {
public:
    void recordPulled(std::int64_t objects, std::int64_t bytes) {
        m_pulledObjects.store(objects, std::memory_order_relaxed);
        m_pulledBytes.store(bytes, std::memory_order_relaxed);
    }
    void recordSent(std::int64_t objects, std::int64_t bytes) {
        m_sentObjects.store(objects, std::memory_order_relaxed);
        m_sentBytes.store(bytes, std::memory_order_relaxed);
    }
    // "pulled_objects=N pulled_bytes=B sent_objects=N sent_bytes=B", with
    // "unknown" for a sent count moqxr did not give.
    QString finalFields() const {
        const auto sent = [](std::int64_t value) {
            return value < 0 ? QStringLiteral("unknown") : QString::number(value);
        };
        return QStringLiteral("pulled_objects=%1 pulled_bytes=%2 sent_objects=%3 sent_bytes=%4")
            .arg(m_pulledObjects.load(std::memory_order_relaxed))
            .arg(m_pulledBytes.load(std::memory_order_relaxed))
            .arg(sent(m_sentObjects.load(std::memory_order_relaxed)), sent(m_sentBytes.load(std::memory_order_relaxed)));
    }

private:
    std::atomic<std::int64_t> m_pulledObjects{0};
    std::atomic<std::int64_t> m_pulledBytes{0};
    std::atomic<std::int64_t> m_sentObjects{-1};
    std::atomic<std::int64_t> m_sentBytes{-1};
};

class IMoqOutput {
public:
    virtual ~IMoqOutput() = default;
    virtual bool connect(const PublishConfig& cfg) = 0;
    virtual bool publishLiveObjects(const PublishConfig& cfg,
                                    const QString& mediaTrackName,
                                    const QStringList& sideTrackNames,
                                    const QByteArray& catalog,
                                    std::function<std::optional<PublishedObject>()> nextObject,
                                    std::atomic<bool>& running) = 0;
    virtual void stop() = 0;
    virtual bool connected() const = 0;
};

class MoqxrPublisher final : public QObject, public IMoqOutput {
    Q_OBJECT
public:
    explicit MoqxrPublisher(QObject* parent = nullptr);
    ~MoqxrPublisher() override;

    bool connect(const PublishConfig& cfg) override;
    bool publishLiveObjects(const PublishConfig& cfg,
                            const QString& mediaTrackName,
                            const QStringList& sideTrackNames,
                            const QByteArray& catalog,
                            std::function<std::optional<PublishedObject>()> nextObject,
                            std::atomic<bool>& running) override;
    void stop() override;
    bool connected() const override;

    // The MOQT drafts that --draft accepts with the pinned moqxr SDK.
    // A relay accepts only the drafts in its config, so an
    // unsupported value is refused rather than replaced by another draft.
    static bool supportsDraft(int draft);
    static QString supportedDrafts();

    // True when the moqxr built against has
    // PublisherConfig::await_subscriber_indefinitely, which a live source
    // sets: the wait for a subscriber then has no deadline.
    static bool awaitsSubscriberIndefinitely();
    // PublisherConfig::subscriber_timeout. A live source waits for as long as
    // it runs: subscribers come and go, and the time without one has no
    // bound. With await_subscriber_indefinitely, the timeout only bounds
    // request-stream reads and keeps the moqxr default, 30 s. Without it
    // (moqxr v0.4.8 and v0.4.10), moqxr ends the publish with success once
    // the timeout passes without a subscriber, and has no "no timeout" value
    // (0 ends the wait at once), so it is 10 years, which still fits a
    // steady_clock deadline. A file keeps a bounded wait.
    static std::chrono::seconds subscriberTimeout(bool liveSource);
    // Why a publish that moqxr ended without an error ended too soon, or empty
    // when it ended as moq2ts asked: running turned false, publisherStopped
    // (stop() ended it, as the CLI does first on SIGINT/SIGTERM, while running
    // is still true), or sourceEnded (the source had no more Objects). Only a
    // live source ends too soon: a file publish ends after its bounded wait,
    // as before.
    static QString unexpectedEndReason(bool liveSource, bool running, bool publisherStopped, bool sourceEnded);

signals:
    void connectionStateChanged(bool connected, const QString& message);
    // Each Object handed to moqxr, with the running totals: what was
    // pulled, which moqxr may drop while nobody subscribes.
    void framePublished(const QString& track, int64_t bytes, int64_t objects);
    // What moqxr wrote, from its statistics, once a publish ends without an
    // error. Not emitted after a failed publish: moqxr then keeps no count.
    void sentTotals(int64_t objects, int64_t bytes);
    void publishError(const QString& error);

private:
    bool connectMock(const PublishConfig& cfg);
    bool publishLiveObjectsMock(const QByteArray& catalog,
                                std::function<std::optional<PublishedObject>()> nextObject,
                                std::atomic<bool>& running,
                                int pacingMs);

    bool m_connected = false;
    QString m_namespace;
    QString m_endpoint;
    mutable QMutex m_mutex;
#ifdef MOQ2TS_HAS_MOQXR
    std::shared_ptr<openmoq::publisher::Publisher> m_activePublisher;
#endif
};

} // namespace moq2ts
