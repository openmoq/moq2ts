#include "MoqxrPublisher.h"
#include "ObjectLookahead.h"

#include <QDebug>
#include <QMutexLocker>
#include <QThread>

#include <chrono>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#ifdef MOQ2TS_HAS_MOQXR
#include "openmoq/publisher/live_object.h"
#include "openmoq/publisher/moq_draft.h"
#include "openmoq/publisher/publisher_api.h"
#endif

namespace {

bool useMockTransport(const moq2ts::PublishConfig& cfg) {
    return cfg.moqEndpoint.trimmed().startsWith(QStringLiteral("mock://"), Qt::CaseInsensitive);
}

#ifdef MOQ2TS_HAS_MOQXR
openmoq::publisher::transport::EndpointConfig parseEndpoint(const QString& rawEndpoint) {
    using namespace openmoq::publisher::transport;

    EndpointConfig endpoint;
    std::string authority = rawEndpoint.trimmed().toStdString();
    const auto consumeScheme = [&](const char* prefix) {
        const std::string value(prefix);
        if (authority.rfind(value, 0) == 0) {
            authority = authority.substr(value.size());
            return true;
        }
        return false;
    };

    const bool hadMoqt = consumeScheme("moqt://");
    const bool hadHttps = consumeScheme("https://");
    const bool hadHttp = consumeScheme("http://");
    if (hadMoqt || hadHttps || hadHttp) {
        const std::size_t slash = authority.find('/');
        if (slash != std::string::npos) {
            endpoint.path = authority.substr(slash);
            endpoint.path_explicit = true;
            authority = authority.substr(0, slash);
        } else if (hadHttps || hadHttp) {
            endpoint.path.clear();
            endpoint.path.push_back('/');
            endpoint.path_explicit = true;
        }
        endpoint.transport = (hadHttps || hadHttp) ? TransportKind::kWebTransport : TransportKind::kRawQuic;
    }

    const std::size_t colon = authority.rfind(':');
    if (colon == std::string::npos || colon == 0 || colon + 1 >= authority.size()) {
        throw std::runtime_error("endpoint must be host:port, moqt://host:port/path, or https://host:port/path");
    }
    endpoint.host = authority.substr(0, colon);
    const int port = std::stoi(authority.substr(colon + 1));
    if (port <= 0 || port > 65535) {
        throw std::runtime_error("endpoint port must be between 1 and 65535");
    }
    endpoint.port = static_cast<std::uint16_t>(port);
    if (endpoint.transport == TransportKind::kWebTransport && !endpoint.path_explicit) {
        endpoint.path.clear();
        endpoint.path.push_back('/');
        endpoint.path.append("moq");
        endpoint.path_explicit = true;
    }
    return endpoint;
}

std::vector<std::uint8_t> toVector(const QByteArray& bytes) {
    const auto* begin = reinterpret_cast<const std::uint8_t*>(bytes.constData());
    return std::vector<std::uint8_t>(begin, begin + bytes.size());
}

// Map the user-selected transport draft to the moqxr API without silently
// falling back to another version.
std::optional<openmoq::publisher::DraftVersion> toDraftVersion(int draft) {
    switch (draft) {
    case 14: return openmoq::publisher::DraftVersion::kDraft14;
    case 16: return openmoq::publisher::DraftVersion::kDraft16;
    case 17: return openmoq::publisher::DraftVersion::kDraft17;
    case 18: return openmoq::publisher::DraftVersion::kDraft18;
    case 21: return openmoq::publisher::DraftVersion::kDraft21;
    case 22: return openmoq::publisher::DraftVersion::kDraft22;
    default: return std::nullopt;
    }
}

// PublisherConfig::await_subscriber_indefinitely is newer than moqxr v0.4.10:
// set it only when the moqxr built against has it. A template, so that the
// discarded branch is not checked against a Config without the field.
template <typename Config>
constexpr bool kHasAwaitSubscriberIndefinitely = requires(Config config) { config.await_subscriber_indefinitely; };

template <typename Config>
void setAwaitSubscriberIndefinitely(Config& config, bool enabled) {
    if constexpr (kHasAwaitSubscriberIndefinitely<Config>) {
        config.await_subscriber_indefinitely = enabled;
    }
}

#endif

} // namespace

namespace moq2ts {

MoqxrPublisher::MoqxrPublisher(QObject* parent)
    : QObject(parent) {}

MoqxrPublisher::~MoqxrPublisher() {
    stop();
}

bool MoqxrPublisher::connect(const PublishConfig& cfg) {
    QMutexLocker locker(&m_mutex);
    m_endpoint = cfg.moqEndpoint;
    m_namespace = cfg.namespaceName;

#ifdef MOQ2TS_HAS_MOQXR
    if (useMockTransport(cfg)) {
        return connectMock(cfg);
    }

    m_connected = true;
    emit connectionStateChanged(true, QString("moqxr publisher configured for %1; namespace=%2").arg(m_endpoint, m_namespace));
    return true;
#else
    if (!useMockTransport(cfg)) {
        emit publishError("Mock publisher builds only accept mock:// endpoints. Set MOQ2TS_BUILD_WITH_MOCK_MOQXR=OFF and rebuild to publish to a real relay.");
        return false;
    }
    return connectMock(cfg);
#endif
}

bool MoqxrPublisher::connectMock(const PublishConfig& cfg) {
    if (cfg.moqEndpoint.isEmpty() || cfg.namespaceName.isEmpty()) {
        emit publishError("Invalid MOQ endpoint/namespace configuration.");
        return false;
    }

    m_connected = true;
    emit connectionStateChanged(true, QString("Mock publisher connected to %1; namespace=%2").arg(m_endpoint, m_namespace));
    return true;
}

bool MoqxrPublisher::publishLiveObjects(const PublishConfig& cfg,
                                        const QString& mediaTrackName,
                                        const QStringList& sideTrackNames,
                                        const QByteArray& catalog,
                                        std::function<std::optional<PublishedObject>()> nextObject,
                                        std::atomic<bool>& running) {
#ifdef MOQ2TS_HAS_MOQXR
    {
        QMutexLocker locker(&m_mutex);
        if (!m_connected) {
            emit publishError("Cannot publish: no active moqxr session.");
            return false;
        }
    }

    if (useMockTransport(cfg)) {
        return publishLiveObjectsMock(catalog, std::move(nextObject), running, cfg.fragmentDurationMs);
    }

    try {
        openmoq::publisher::PublisherConfig publisherConfig;
        // Never fall back to another draft: the relay would refuse it at the
        // ALPN, with an error that does not name the cause.
        const std::optional<openmoq::publisher::DraftVersion> draft = toDraftVersion(cfg.draftVersion);
        if (!draft.has_value()) {
            emit publishError(QStringLiteral("Unsupported MOQ draft %1; use %2.")
                                  .arg(cfg.draftVersion)
                                  .arg(supportedDrafts()));
            return false;
        }
        publisherConfig.draft_version = *draft;
        publisherConfig.track_namespace = cfg.namespaceName.toStdString();
        // Await-subscribe mode: only send data after the relay forwards a
        // SUBSCRIBE from a downstream subscriber. The moqxr library no longer
        // pre-announces tracks with PUBLISH messages, so the relay must forward
        // actual SUBSCRIBE messages - matching the moqxr SRT ingest flow.
        publisherConfig.forward = false;
        publisherConfig.publish_catalog = false;
        publisherConfig.paced = false;
        publisherConfig.loop = false;
        publisherConfig.subscriber_timeout = subscriberTimeout(cfg.liveSource);
        setAwaitSubscriberIndefinitely(publisherConfig, cfg.liveSource);

        openmoq::publisher::LiveObjectSource source;
        source.tracks.push_back({.track_name = kCatalogTrackName});
        source.tracks.push_back({.track_name = mediaTrackName.toStdString()});
        for (const QString& trackName : sideTrackNames) {
            source.tracks.push_back({.track_name = trackName.toStdString()});
        }

        bool catalogSent = false;
        // Set once the source has no more Objects: the publish then ends as
        // moq2ts asked.
        auto sourceEnded = std::make_shared<std::atomic<bool>>(false);
        int64_t publisherObjects = 0;
        int64_t publisherBytes = 0;
        // final_in_subgroup must mark the last object of a group ON ITS OWN
        // TRACK, and next_object() interleaves the media, catalog and timeline
        // tracks (and, for a multi-program source, several media tracks).
        // ObjectLookahead holds back one object per track until the next object
        // of that track shows whether its group ended, so a track that stalls
        // never holds back another track's objects.
        auto lookahead = std::make_shared<ObjectLookahead>(std::move(nextObject));
        source.next_object = [this,
                              catalogSent,
                              publisherObjects,
                              publisherBytes,
                              catalog,
                              lookahead,
                              sourceEnded]() mutable -> std::optional<openmoq::publisher::LiveObject> {
            if (!catalogSent) {
                catalogSent = true;
                ++publisherObjects;
                publisherBytes += catalog.size();
                emit framePublished(QStringLiteral("catalog"), publisherBytes, publisherObjects);
                return openmoq::publisher::LiveObject{
                    .track_name = kCatalogTrackName,
                    .group_id = 0,
                    .subgroup_id = 0,
                    .object_id = 0,
                    .media_time_us = 0,
                    .media_duration_us = 0,
                    .payload = toVector(catalog),
                    .subgroup_contains_group_largest = true,
                    .final_in_subgroup = true,
                };
            }

            std::optional<ReadyObject> ready = lookahead->next();
            if (!ready.has_value()) {
                sourceEnded->store(true, std::memory_order_release);
                return std::nullopt;
            }
            const PublishedObject& current = ready->object;
            ++publisherObjects;
            publisherBytes += current.payload.size();
            emit framePublished(current.trackName, publisherBytes, publisherObjects);

            return openmoq::publisher::LiveObject{
                .track_name = current.trackName.toStdString(),
                .group_id = static_cast<std::size_t>(current.groupId),
                // One subgroup per Group: see subgroup_contains_group_largest.
                .subgroup_id = 0,
                .object_id = static_cast<std::size_t>(current.objectId),
                .media_time_us = current.mediaTimeUs,
                .media_duration_us = current.mediaDurationUs,
                .payload = toVector(current.payload),
                // One subgroup per Group, so it holds the Group's largest
                // Object. moqxr writes the flag into the subgroup header from
                // the first Object, and the subgroup FIN then ends the Group.
                .subgroup_contains_group_largest = true,
                .final_in_subgroup = ready->finalInGroup,
                .ends_broadcast = ready->object.endsBroadcast,
            };
        };

        auto activePublisher = std::make_shared<openmoq::publisher::Publisher>(publisherConfig);
        {
            QMutexLocker locker(&m_mutex);
            m_activePublisher = activePublisher;
        }

        openmoq::publisher::transport::TlsConfig tls;
        tls.insecure_skip_verify = cfg.insecure;
        const auto status = activePublisher->publish_live_objects(source, parseEndpoint(cfg.moqEndpoint), tls, false);
        // A stop request: running turned false, or stop() ended the publish.
        // The CLI stops the publisher before the pipeline on SIGINT/SIGTERM,
        // so running can still be true here after an interrupt.
        bool publisherStopped = false;
        {
            QMutexLocker locker(&m_mutex);
            publisherStopped = !m_connected;
        }
        if (!status.ok && running.load(std::memory_order_acquire) && !publisherStopped) {
            emit publishError(QString::fromStdString(status.message));
            QMutexLocker locker(&m_mutex);
            if (m_activePublisher == activePublisher) {
                m_activePublisher.reset();
            }
            return false;
        }
        // Only one thread disconnects a Publisher. stop() takes m_activePublisher
        // first when it interrupts a publish; the disconnect here is for the
        // stream that ended on its own.
        bool ownsDisconnect = false;
        {
            QMutexLocker locker(&m_mutex);
            if (m_activePublisher == activePublisher) {
                m_activePublisher.reset();
                ownsDisconnect = true;
            }
        }
        if (ownsDisconnect) {
            const auto disconnectStatus = activePublisher->disconnect(0);
            if (!disconnectStatus.ok) {
                emit publishError(QString::fromStdString(disconnectStatus.message));
                return false;
            }
        }

        const auto stats = activePublisher->stats();
        // moqxr copies its session counts into stats() only when the publish
        // succeeds; after a failure they are not what it sent.
        if (status.ok) {
            emit sentTotals(static_cast<int64_t>(stats.objects_published), static_cast<int64_t>(stats.bytes_published));
        }
        const QString unexpectedEnd = unexpectedEndReason(cfg.liveSource, running.load(std::memory_order_acquire),
                                                          publisherStopped,
                                                          sourceEnded->load(std::memory_order_acquire));
        if (!unexpectedEnd.isEmpty()) {
            qWarning("%s (%llu Objects sent%s%s).", qUtf8Printable(unexpectedEnd),
                     static_cast<unsigned long long>(stats.objects_published),
                     stats.last_error.empty() ? "" : "; last error: ", stats.last_error.c_str());
            return false;
        }
        return status.ok;
    } catch (const std::exception& error) {
        QMutexLocker locker(&m_mutex);
        m_activePublisher.reset();
        emit publishError(QString::fromStdString(error.what()));
        return false;
    }
#else
    Q_UNUSED(cfg);
    Q_UNUSED(mediaTrackName);
    Q_UNUSED(sideTrackNames);
    return publishLiveObjectsMock(catalog, std::move(nextObject), running, cfg.fragmentDurationMs);
#endif
}

bool MoqxrPublisher::publishLiveObjectsMock(const QByteArray& catalog,
                                            std::function<std::optional<PublishedObject>()> nextObject,
                                            std::atomic<bool>& running,
                                            int pacingMs) {
    {
        QMutexLocker locker(&m_mutex);
        if (!m_connected) {
            emit publishError("Cannot publish: mock publisher not connected.");
            return false;
        }
    }

    qDebug() << "MOCK MSFTS catalog bytes=" << catalog.size();
    int64_t publisherObjects = 1;
    int64_t publisherBytes = catalog.size();
    emit framePublished(QStringLiteral("catalog"), publisherBytes, publisherObjects);

    // The same look-ahead as the real path, so the mock logs what it would
    // send. It sends what it pulls.
    ObjectLookahead lookahead(std::move(nextObject));
    while (running.load(std::memory_order_acquire)) {
        std::optional<ReadyObject> ready = lookahead.next();
        if (!ready.has_value()) {
            break;
        }
        const PublishedObject* object = &ready->object;
        qDebug() << "MOCK MSFTS publish" << object->trackName << "group=" << object->groupId
                 << "object=" << object->objectId << "bytes=" << object->payload.size()
                 << "final=" << ready->finalInGroup
                 << (object->endsBroadcast ? "ends_broadcast" : "");
        ++publisherObjects;
        publisherBytes += object->payload.size();
        emit framePublished(object->trackName, publisherBytes, publisherObjects);
        if (pacingMs > 0) {
            QThread::msleep(static_cast<unsigned long>(pacingMs));
        }
    }
    emit sentTotals(publisherObjects, publisherBytes);
    return true;
}


void MoqxrPublisher::stop() {
#ifdef MOQ2TS_HAS_MOQXR
    std::shared_ptr<openmoq::publisher::Publisher> publisherToStop;
#endif
    {
        QMutexLocker locker(&m_mutex);
        if (!m_connected) {
            return;
        }
        m_connected = false;
#ifdef MOQ2TS_HAS_MOQXR
        // stop() takes the publisher: the pipeline thread, which sees it gone,
        // then does not disconnect it a second time while this one still does.
        publisherToStop = std::move(m_activePublisher);
        m_activePublisher.reset();
#endif
    }
#ifdef MOQ2TS_HAS_MOQXR
    if (publisherToStop) {
        const auto disconnectStatus = publisherToStop->disconnect(0);
        if (!disconnectStatus.ok) {
            emit publishError(QString::fromStdString(disconnectStatus.message));
        }
    }
#endif
    emit connectionStateChanged(false, "Publisher stopped");
}

bool MoqxrPublisher::connected() const {
    QMutexLocker locker(&m_mutex);
    return m_connected;
}

bool MoqxrPublisher::supportsDraft(int draft) {
    return draft == 14 || draft == 16 || draft == 17 || draft == 18 || draft == 21 || draft == 22;
}

QString MoqxrPublisher::supportedDrafts() {
    return QStringLiteral("14, 16, 17, 18, 21 or 22");
}

bool MoqxrPublisher::awaitsSubscriberIndefinitely() {
#ifdef MOQ2TS_HAS_MOQXR
    return kHasAwaitSubscriberIndefinitely<openmoq::publisher::PublisherConfig>;
#else
    return false;
#endif
}

std::chrono::seconds MoqxrPublisher::subscriberTimeout(bool liveSource) {
    if (!liveSource) {
        return std::chrono::seconds(120);
    }
    return awaitsSubscriberIndefinitely() ? std::chrono::seconds(30) : std::chrono::hours(24 * 365 * 10);
}

QString MoqxrPublisher::unexpectedEndReason(bool liveSource, bool running, bool publisherStopped, bool sourceEnded) {
    if (!liveSource || !running || publisherStopped || sourceEnded) {
        return {};
    }
    // moqxr gives no reason with a success: with no subscriber timeout left
    // for a live source, its loop ends on its own when the relay ends the
    // control stream, but other causes cannot be ruled out from here.
    return QStringLiteral("The MOQT session ended while the live source was still running, without an error from "
                          "moqxr (the relay may have closed the session); the publish stops");
}

} // namespace moq2ts
