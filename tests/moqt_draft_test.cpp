#include "app/PublishConfig.h"
#include "publish/MoqxrPublisher.h"

#include <atomic>
#include <chrono>
#include <iostream>
#include <string>
#include <optional>

int main() {
    bool ok = true;
    const auto check = [&ok](bool condition, const std::string& message) {
        if (!condition) {
            std::cerr << "FAIL: " << message << '\n';
            ok = false;
        }
    };

    check(moq2ts::PublishConfig{}.draftVersion == 22, "new sessions default to MOQT draft 22");
    check(moq2ts::MoqxrPublisher::supportsDraft(22), "draft 22 is accepted");
    check(moq2ts::MoqxrPublisher::supportsDraft(21), "draft 21 is accepted");
    check(moq2ts::MoqxrPublisher::supportsDraft(18), "draft 18 remains accepted");
    check(!moq2ts::MoqxrPublisher::supportsDraft(20), "unsupported draft 20 is rejected");
    check(!moq2ts::MoqxrPublisher::supportsDraft(23), "unknown draft 23 is rejected");

    // A live source keeps waiting for subscribers: moqxr ends a publish that
    // waited subscriber_timeout for one, and a live publisher must outlast
    // any time without a subscriber. A file keeps its bounded wait.
    using moq2ts::MoqxrPublisher;
    if (MoqxrPublisher::awaitsSubscriberIndefinitely()) {
        // A moqxr with PublisherConfig::await_subscriber_indefinitely: the
        // wait has no deadline, and the timeout only bounds request-stream
        // reads, so it keeps the moqxr default.
        check(MoqxrPublisher::subscriberTimeout(true) == std::chrono::seconds(30),
              "with an unlimited wait, a live source keeps moqxr's 30 s timeout for request-stream reads");
    } else {
        check(MoqxrPublisher::subscriberTimeout(true) >= std::chrono::hours(24 * 365),
              "a live source waits for subscribers without a practical bound");
    }
    check(MoqxrPublisher::subscriberTimeout(false) == std::chrono::seconds(120),
          "a file source keeps its 120 s wait for a subscriber");
    // moqxr adds the timeout to steady_clock::now(), in nanoseconds: the sum
    // must not overflow.
    check(MoqxrPublisher::subscriberTimeout(true) <
              std::chrono::duration_cast<std::chrono::seconds>(std::chrono::nanoseconds::max()) / 4,
          "the live wait does not overflow a steady_clock deadline");

    // A publish that moqxr ends without an error is a failure for a live
    // source, unless moq2ts asked for it: a stop, or the end of the source.
    // Arguments: live source, running, publisher stopped, source ended.
    const QString early = MoqxrPublisher::unexpectedEndReason(true, true, false, false);
    check(!early.isEmpty(), "a live publish that ends on its own has a reason");
    check(early.contains(QStringLiteral("live source")), "the reason says the live source was still running");
    check(MoqxrPublisher::unexpectedEndReason(true, false, false, false).isEmpty(), "a stop is no unexpected end");
    // SIGINT/SIGTERM on the --config path: MoqxrPublisher::stop() ends the
    // publish before the pipeline clears running.
    check(MoqxrPublisher::unexpectedEndReason(true, true, true, false).isEmpty(),
          "an interrupt that stops the publisher first is no unexpected end");
    check(MoqxrPublisher::unexpectedEndReason(true, true, false, true).isEmpty(),
          "the end of the source is no unexpected end");
    check(MoqxrPublisher::unexpectedEndReason(false, true, false, false).isEmpty(), "a file publish keeps its behaviour");

    // The [final] line keeps what the publisher pulled apart from what moqxr
    // sent: a run without a subscriber pulls everything and sends nothing.
    {
        moq2ts::PublishTotals totals;
        check(totals.finalFields() == "pulled_objects=0 pulled_bytes=0 sent_objects=unknown sent_bytes=unknown",
              "before any report, the sent counts are unknown");
        totals.recordPulled(12, 34);
        totals.recordSent(5, 6);
        check(totals.finalFields() == "pulled_objects=12 pulled_bytes=34 sent_objects=5 sent_bytes=6",
              "pulled and sent are reported apart: " + totals.finalFields().toStdString());

        // The mock publisher sends what it pulls: the catalog and three Objects.
        moq2ts::PublishTotals mock;
        moq2ts::PublishConfig cfg;
        cfg.moqEndpoint = QStringLiteral("mock://local");
        cfg.fragmentDurationMs = 0;
        MoqxrPublisher publisher;
        QObject::connect(&publisher, &MoqxrPublisher::framePublished, &publisher,
                         [&mock](const QString&, int64_t bytes, int64_t objects) { mock.recordPulled(objects, bytes); },
                         Qt::DirectConnection);
        QObject::connect(&publisher, &MoqxrPublisher::sentTotals, &publisher,
                         [&mock](int64_t objects, int64_t bytes) { mock.recordSent(objects, bytes); },
                         Qt::DirectConnection);
        publisher.connect(cfg);
        int left = 3;
        std::atomic<bool> running{true};
        const bool published = publisher.publishLiveObjects(
            cfg, QStringLiteral("program-1"), {}, QByteArray(10, 'c'),
            [&left]() -> std::optional<moq2ts::PublishedObject> {
                if (left == 0) {
                    return std::nullopt;
                }
                --left;
                moq2ts::PublishedObject object;
                object.trackName = QStringLiteral("program-1");
                object.groupId = static_cast<std::uint64_t>(3 - left);
                object.startsGroup = true;
                object.payload = QByteArray(100, 'x');
                return object;
            },
            running);
        check(published, "mock publish succeeds");
        check(mock.finalFields() == "pulled_objects=4 pulled_bytes=310 sent_objects=4 sent_bytes=310",
              "the mock reports what it sent: " + mock.finalFields().toStdString());
    }
    return ok ? 0 : 1;
}
