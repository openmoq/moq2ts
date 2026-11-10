// The real moqxr publish path against a relay that cannot be resolved: a
// live publish gives up after the bound of ReconnectPolicy and reports why.
// Slow: the waits add up to 90 s.
#include "app/PublishConfig.h"
#include "publish/MoqxrPublisher.h"

#include <atomic>
#include <iostream>
#include <optional>

int main() {
    moq2ts::PublishConfig cfg;
    cfg.moqEndpoint = QStringLiteral("moqt://nonexistent.invalid:4433/moq");
    cfg.namespaceName = QStringLiteral("test/unreachable");
    cfg.liveSource = true;
    moq2ts::MoqxrPublisher publisher;
    QString error;
    QObject::connect(&publisher, &moq2ts::MoqxrPublisher::publishError,
                     [&error](const QString& message) { error = message; });
    publisher.connect(cfg);
    std::atomic<bool> running{true};
    const bool published = publisher.publishLiveObjects(
        cfg, QStringLiteral("program-1"), {}, QByteArray(10, 'c'),
        []() -> std::optional<moq2ts::PublishedObject> { return std::nullopt; }, running);
    bool ok = true;
    if (published) {
        std::cerr << "FAIL: the publish to an unresolvable relay succeeded\n";
        ok = false;
    }
    if (!error.contains(QStringLiteral("giving up after 10 attempts"))) {
        std::cerr << "FAIL: the error does not say that the publish gave up: " << error.toStdString() << '\n';
        ok = false;
    }
    return ok ? 0 : 1;
}
