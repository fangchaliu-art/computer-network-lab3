// Local loopback integration tests; no Router or file transfer is involved.
#include "../udp_io.h"
#include "../config.h"

#include <chrono>
#include <iostream>
#include <stdexcept>
#include <system_error>

namespace {
int passed = 0;

void check(bool condition, const char* label) {
    if (!condition) throw std::runtime_error(label);
    std::cout << "[PASS] " << label << '\n';
    ++passed;
}

template<class Exception, class Action>
void expect_error(Action action, const char* label) {
    try {
        action();
    } catch (const Exception&) {
        check(true, label);
        return;
    }
    check(false, label);
}
} // namespace

int main() {
    using namespace lab3;
    UdpSocket sender = kInvalidUdpSocket;
    UdpSocket receiver = kInvalidUdpSocket;
    bool initialized = false;
    try {
        udp_initialize();
        initialized = true;
        sender = udp_open_bound({"127.0.0.1", 0});
        receiver = udp_open_bound({"127.0.0.1", 0});
        const auto a = udp_local_endpoint(sender);
        const auto b = udp_local_endpoint(receiver);
        check(a.port != 0 && b.port != 0 && a.port != b.port,
              "bind port 0 assigns distinct local ports");

        const std::vector<std::uint8_t> binary{0, 1, 127, 128, 255, 0};
        udp_send(sender, b, binary);
        auto result = udp_receive(receiver, 1000);
        check(result.status == UdpReceiveStatus::Received && result.data == binary,
              "binary datagram arrives unchanged");
        check(result.source.ip == a.ip && result.source.port == a.port,
              "recvfrom reports the actual source endpoint");

        const std::vector<std::uint8_t> ack{'A', 'C', 'K'};
        udp_send(receiver, result.source, ack);
        result = udp_receive(sender, 1000);
        check(result.status == UdpReceiveStatus::Received && result.data == ack &&
              result.source.port == b.port, "reverse-direction reply works");

        udp_send(sender, b, {});
        result = udp_receive(receiver, 1000);
        check(result.status == UdpReceiveStatus::Received && result.data.empty(),
              "empty datagram is received rather than classified as timeout");

        std::vector<std::uint8_t> maximum(kRouterMaxPacketBytes);
        for (std::size_t i = 0; i < maximum.size(); ++i)
            maximum[i] = static_cast<std::uint8_t>(i % 256);
        udp_send(sender, b, maximum);
        result = udp_receive(receiver, 1000);
        check(result.status == UdpReceiveStatus::Received && result.data == maximum,
              "15000-byte datagram arrives intact");

        expect_error<std::invalid_argument>([&] {
            udp_send(sender, b, std::vector<std::uint8_t>(kRouterMaxPacketBytes + 1));
        }, "oversized sends are rejected before sendto");

        // Bypass the module's send limit to test hostile/invalid incoming data.
        sockaddr_in destination{};
        destination.sin_family = AF_INET;
        destination.sin_port = htons(b.port);
        inet_pton(AF_INET, b.ip.c_str(), &destination.sin_addr);
        for (const int length : {15001, 16000}) {
            const std::string oversized(static_cast<std::size_t>(length), 'x');
            const auto sent = sendto(sender, oversized.data(), length, 0,
                                     reinterpret_cast<const sockaddr*>(&destination),
                                     sizeof(destination));
            if (sent != length) throw std::runtime_error("raw oversized test send failed");
            result = udp_receive(receiver, 1000);
            check(result.status == UdpReceiveStatus::Oversized && result.data.empty(),
                  length == 15001 ? "15001-byte incoming datagram is discarded"
                                  : "truncated oversized incoming datagram is discarded");
        }
        udp_send(sender, b, binary);
        result = udp_receive(receiver, 1000);
        check(result.status == UdpReceiveStatus::Received && result.data == binary,
              "normal receive still works after oversized datagrams");

        const auto start = std::chrono::steady_clock::now();
        result = udp_receive(receiver, 60);
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start).count();
        check(result.status == UdpReceiveStatus::Timeout && elapsed >= 40 && elapsed < 2000,
              "idle receive returns Timeout near the requested deadline");
        std::cout << "[METRIC] requested_timeout_ms=60 observed_ms=" << elapsed << '\n';
        check(udp_receive(receiver, 0).status == UdpReceiveStatus::Timeout,
              "zero timeout polls without incoming data");
        udp_send(sender, b, ack);
        check(udp_receive(receiver, 1000).data == ack,
              "socket remains usable after a timeout");

        expect_error<std::invalid_argument>([&] { udp_receive(receiver, -1); },
                                            "negative timeout is rejected");
        expect_error<std::invalid_argument>([&] { udp_send(sender, {b.ip, 0}, ack); },
                                            "destination port 0 is rejected");
        expect_error<std::invalid_argument>([&] {
            udp_send(sender, {"bad-ip", b.port}, ack);
        }, "invalid destination IPv4 is rejected");
        expect_error<std::invalid_argument>([] { udp_open_bound({"bad-ip", 0}); },
                                            "invalid bind IPv4 is rejected");
        expect_error<std::system_error>([&] {
            auto duplicate = udp_open_bound(b);
            udp_close(duplicate);
        }, "binding an occupied endpoint reports a native socket error");

        udp_close(sender);
        udp_close(sender);
        check(sender == kInvalidUdpSocket, "close invalidates handle and is repeatable");
        expect_error<std::invalid_argument>([&] { udp_receive(sender, 0); },
                                            "receiving with invalid handle is rejected");
        udp_close(receiver);
        udp_cleanup();
        initialized = false;
        std::cout << "[RESULT] " << passed << " checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        udp_close(sender);
        udp_close(receiver);
        if (initialized) udp_cleanup();
        std::cerr << "[FAIL] " << error.what() << '\n';
        return 1;
    }
}
