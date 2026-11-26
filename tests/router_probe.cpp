// Manual integration probe for the teacher's Router.exe.
// This is not the reliable-transfer protocol: no retries or file transfer.
#include "../udp_io.h"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

namespace {
using Clock = std::chrono::steady_clock;
const lab3::UdpEndpoint router{"127.0.0.1", 9000};

int number(const char* text, int low, int high) {
    const std::string value(text);
    int result = 0;
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
    if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() ||
        result < low || result > high)
        throw std::invalid_argument("numeric argument must be in [" +
            std::to_string(low) + ", " + std::to_string(high) + "]");
    return result;
}

bool from_router(const lab3::UdpEndpoint& source) {
    return source.ip == router.ip && source.port == router.port;
}

void help() {
    std::cout << "Router integration probe (loopback IPv4 only)\n"
                 "  router_probe.exe receive [idle_ms=120000]\n"
                 "  router_probe.exe send [count=20] [reply_timeout_ms=1500] [gap_ms=200]\n"
                 "Router: 127.0.0.1:9000; sender: :9001; receiver: :9002\n"
                 "Configure Router's server endpoint as 127.0.0.1:9002.\n"
                 "No retries. Exit 1 means some probes received no reply by their deadline.\n";
}

void receive_probes(lab3::UdpSocket socket, int idle_ms) {
    std::size_t count = 0;
    std::cout << "[READY] receiver=127.0.0.1:9002 reply_target=127.0.0.1:9000\n"
              << "[READY] waiting; exits after " << idle_ms << "ms without a datagram\n";
    for (;;) {
        const auto result = lab3::udp_receive(socket, idle_ms);
        if (result.status == lab3::UdpReceiveStatus::Timeout) break;
        if (result.status == lab3::UdpReceiveStatus::Oversized) {
            std::cout << "[DROP] oversized datagram\n";
            continue;
        }
        if (!from_router(result.source)) {
            std::cout << "[IGNORE] unexpected source=" << result.source.ip
                      << ':' << result.source.port << '\n';
            continue;
        }
        const std::string text(result.data.begin(), result.data.end());
        if (text.rfind("LAB3_UDP_PROBE ", 0) != 0) {
            std::cout << "[IGNORE] not a probe packet\n";
            continue;
        }
        ++count;
        std::cout << "[RECV] source=" << result.source.ip << ':' << result.source.port
                  << " bytes=" << result.data.size() << " " << text << '\n';
        // Always use the configured router, not the sender's :9001 endpoint.
        lab3::udp_send(socket, router, result.data);
        std::cout << "[REPLY] target=127.0.0.1:9000\n";
    }
    std::cout << "[RESULT] receiver_idle_timeout received=" << count << '\n';
}

int send_probes(lab3::UdpSocket socket, int count, int timeout_ms, int gap_ms) {
    // Distinguish separate runs so a delayed old reply cannot count as a new one.
    const auto session = std::to_string(Clock::now().time_since_epoch().count());
    int replies = 0;
    double total_rtt_ms = 0;
    std::cout << "[READY] sender=127.0.0.1:9001 target=127.0.0.1:9000\n"
              << "[READY] count=" << count << " reply_timeout_ms=" << timeout_ms
              << " gap_ms=" << gap_ms << " retries=0\n";
    for (int seq = 0; seq < count; ++seq) {
        const auto text = "LAB3_UDP_PROBE session=" + session + " seq=" + std::to_string(seq);
        const std::vector<std::uint8_t> data(text.begin(), text.end());
        const auto sent_at = Clock::now();
        lab3::udp_send(socket, router, data);
        std::cout << "[SEND] seq=" << seq << " bytes=" << data.size() << '\n';
        const auto deadline = sent_at + std::chrono::milliseconds(timeout_ms);
        bool matched = false;
        while (Clock::now() < deadline) {
            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - Clock::now()).count();
            const auto result = lab3::udp_receive(socket, static_cast<int>(std::max<decltype(remaining)>(remaining, 1)));
            if (result.status == lab3::UdpReceiveStatus::Timeout) break;
            if (result.status != lab3::UdpReceiveStatus::Received ||
                !from_router(result.source) || result.data != data) {
                std::cout << "[IGNORE] unmatched, oversized or late reply\n";
                continue;
            }
            const auto rtt_ms = std::chrono::duration<double, std::milli>(Clock::now() - sent_at).count();
            ++replies;
            total_rtt_ms += rtt_ms;
            matched = true;
            std::cout << "[REPLY] seq=" << seq << " source=" << result.source.ip
                      << ':' << result.source.port << " rtt_ms=" << rtt_ms << '\n';
            break;
        }
        if (!matched) std::cout << "[TIMEOUT] seq=" << seq << " no reply within " << timeout_ms << "ms\n";
        if (seq + 1 < count) std::this_thread::sleep_for(std::chrono::milliseconds(gap_ms));
    }
    std::cout << "[RESULT] sent=" << count << " replies=" << replies
              << " unanswered=" << count - replies
              << " reply_percent=" << 100.0 * replies / count;
    if (replies > 0) std::cout << " avg_rtt_ms=" << total_rtt_ms / replies;
    else std::cout << " avg_rtt_ms=N/A";
    std::cout << "\n[NOTE] Unanswered means no matching reply before the deadline; "
                 "it is not a direct measurement of Router's drop probability.\n";
    return replies == count ? 0 : 1;
}
} // namespace

int main(int argc, char* argv[]) {
    using namespace lab3;
    UdpSocket socket = kInvalidUdpSocket;
    bool initialized = false;
    std::cout << std::unitbuf << std::fixed << std::setprecision(2);
    try {
        if (argc == 1 || (argc == 2 && std::string(argv[1]) == "--help")) {
            help();
            return 0;
        }
        const std::string mode = argv[1];
        const bool receive = mode == "receive";
        if ((!receive && mode != "send") || (receive && argc > 3) || (!receive && argc > 5))
            throw std::invalid_argument("invalid mode or argument count; use --help");
        int idle_ms = 120000, count = 20, timeout_ms = 1500, gap_ms = 200;
        if (receive) {
            if (argc > 2) idle_ms = number(argv[2], 1, 3600000);
        } else {
            if (argc > 2) count = number(argv[2], 1, 10000);
            if (argc > 3) timeout_ms = number(argv[3], 1, 60000);
            if (argc > 4) gap_ms = number(argv[4], 0, 60000);
        }
        udp_initialize();
        initialized = true;
        socket = udp_open_bound({"127.0.0.1", static_cast<std::uint16_t>(receive ? 9002 : 9001)});
        int code = 0;
        if (receive) receive_probes(socket, idle_ms);
        else code = send_probes(socket, count, timeout_ms, gap_ms);
        udp_close(socket);
        udp_cleanup();
        return code;
    } catch (const std::exception& error) {
        udp_close(socket);
        if (initialized) udp_cleanup();
        std::cerr << "[ERROR] " << error.what() << '\n';
        return 2;
    }
}
