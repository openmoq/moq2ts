// The real moqxr publish path with --quic-idle-timeout: a relay that never
// answers fails the connection attempt within the idle timeout, where
// moqxr's own handshake wait is 5 s.
#include "app/PublishConfig.h"
#include "publish/MoqxrPublisher.h"

#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <iostream>
#include <optional>

int main() {
    // A UDP port on ::1 that receives and never answers.
    const int fd = ::socket(AF_INET6, SOCK_DGRAM, 0);
    sockaddr_in6 address{};
    address.sin6_family = AF_INET6;
    address.sin6_addr = in6addr_loopback;
    socklen_t length = sizeof(address);
    if (fd < 0 || ::bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 ||
        ::getsockname(fd, reinterpret_cast<sockaddr*>(&address), &length) != 0) {
        std::cerr << "FAIL: cannot bind a UDP socket on ::1\n";
        return 1;
    }
    moq2ts::PublishConfig cfg;
    cfg.moqEndpoint = QStringLiteral("moqt://[::1]:%1/moq").arg(ntohs(address.sin6_port));
    cfg.namespaceName = QStringLiteral("test/idle");
    cfg.liveSource = true;
    cfg.reconnect = false;
    cfg.insecure = true;
    cfg.quicIdleTimeoutMs = 1000;
    moq2ts::MoqxrPublisher publisher;
    publisher.connect(cfg);
    std::atomic<bool> running{true};
    const auto started = std::chrono::steady_clock::now();
    const bool published = publisher.publishLiveObjects(
        cfg, QStringLiteral("program-1"), {}, QByteArray(10, 'c'),
        []() -> std::optional<moq2ts::PublishedObject> { return std::nullopt; }, running);
    const auto elapsed = std::chrono::steady_clock::now() - started;
    ::close(fd);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
    if (published || elapsed >= std::chrono::seconds(3)) {
        std::cerr << "FAIL: the attempt to a silent relay took " << ms << " ms (published=" << published
                  << "); with a 1000 ms idle timeout it fails within about 1 s\n";
        return 1;
    }
    std::cout << "quic idle timeout test passed (" << ms << " ms)\n";
    return 0;
}
