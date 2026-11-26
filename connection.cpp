#include "connection.h"

#include <algorithm>
#include <chrono>
#include <random>

namespace lab3 {
namespace {
using Clock = std::chrono::steady_clock;
using Time = Clock::time_point;

void log(const ConnectionOptions& options, const std::string& text) {
    if (options.log) *options.log << text << '\n';
}

void state(Connection& c, ConnectionState next, const ConnectionOptions& options) {
    log(options, std::string("[STATE] ") + connection_state_name(c.state) + " -> " +
        connection_state_name(next) + " session=" + std::to_string(c.session_id));
    c.state = next;
}

void validate_options(const ConnectionOptions& options) {
    if (options.rto_ms == 0 || options.rto_ms > 3600000 || options.max_retries > 10000 ||
        options.idle_timeout_ms < options.rto_ms || options.idle_timeout_ms > 86400000)
        throw ConnectionError("invalid connection timeout/retry options");
}

int remaining_ms(Time deadline) {
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
    return static_cast<int>(std::min<std::int64_t>(std::max<std::int64_t>(remaining, 0), 2147483647));
}

Packet control(const Connection& c, std::uint8_t flags, std::uint32_t ack = 0) {
    Packet packet;
    packet.header.flags = flags;
    packet.header.session_id = c.session_id;
    packet.header.window = c.window;
    packet.header.ack = ack;
    if (flags == kFlagSyn || flags == (kFlagSyn | kFlagAck))
        packet.payload = encode_file_metadata(c.metadata);
    return packet;
}

void send(UdpSocket socket, const UdpEndpoint& router, Connection& c,
          const ConnectionOptions& options, const Packet& packet, bool retry = false) {
    udp_send(socket, router, encode_packet(packet));
    ++c.control_sent;
    if (retry) ++c.retransmissions;
    log(options, std::string(retry ? "[RETRANS] " : "[SEND] ") + packet_summary(packet));
}

// Preserve the caller's deadline even when many malformed or foreign packets arrive.
std::optional<Packet> receive_raw(UdpSocket socket, const UdpEndpoint& router,
                                  const ConnectionOptions& options, Time deadline) {
    bool first = true;
    while (first || Clock::now() < deadline) {
        first = false;
        const auto result = udp_receive(socket, remaining_ms(deadline));
        if (result.status == UdpReceiveStatus::Timeout) return std::nullopt;
        if (result.status == UdpReceiveStatus::Oversized) {
            log(options, "[DROP] oversized datagram");
            continue;
        }
        if (result.source.ip != router.ip || result.source.port != router.port) {
            log(options, "[DROP] unexpected UDP source");
            continue;
        }
        try {
            auto packet = decode_packet(result.data);
            log(options, "[RECV] " + packet_summary(packet));
            return packet;
        } catch (const ProtocolError& error) {
            log(options, std::string("[DROP] ") + error.what());
        }
    }
    return std::nullopt;
}

bool same_metadata(const Packet& packet, const Connection& c) {
    return packet.payload == encode_file_metadata(c.metadata);
}

[[noreturn]] void failed(Connection& c, const ConnectionOptions& options, const std::string& reason) {
    state(c, ConnectionState::Failed, options);
    throw ConnectionError(reason);
}

struct RetryTimer {
    Time next;
    std::uint32_t retries = 0;
    explicit RetryTimer(const ConnectionOptions& o)
        : next(Clock::now() + std::chrono::milliseconds(o.rto_ms)) {}
    void resend(UdpSocket socket, const UdpEndpoint& router, Connection& c,
                const ConnectionOptions& o, const Packet& packet) {
        if (retries >= o.max_retries) failed(c, o, "control packet retry limit exceeded");
        ++retries;
        send(socket, router, c, o, packet, true);
        next = Clock::now() + std::chrono::milliseconds(o.rto_ms);
    }
};

std::optional<Packet> matching(UdpSocket socket, const UdpEndpoint& router, Connection& c,
                               const ConnectionOptions& o, Time deadline) {
    bool first = true;
    while (first || Clock::now() < deadline) {
        first = false;
        auto packet = receive_raw(socket, router, o, deadline);
        if (!packet) return std::nullopt;
        if (packet->header.session_id != c.session_id) {
            log(o, "[DROP] foreign session");
            continue;
        }
        if (packet->header.flags == kFlagRst) failed(c, o, "peer reset the connection");
        if (packet->header.window != c.window) {
            log(o, "[DROP] window mismatch");
            continue;
        }
        return packet;
    }
    return std::nullopt;
}

void require_established(const Connection& c, bool sender) {
    if (c.state != ConnectionState::Established || c.sender != sender)
        throw ConnectionError("operation requires an established connection with the correct role");
}
} // namespace

const char* connection_state_name(ConnectionState s) noexcept {
    switch (s) {
    case ConnectionState::Closed: return "CLOSED";
    case ConnectionState::Listen: return "LISTEN";
    case ConnectionState::SynSent: return "SYN_SENT";
    case ConnectionState::SynReceived: return "SYN_RECEIVED";
    case ConnectionState::Established: return "ESTABLISHED";
    case ConnectionState::FinWait1: return "FIN_WAIT_1";
    case ConnectionState::FinWait2: return "FIN_WAIT_2";
    case ConnectionState::CloseWait: return "CLOSE_WAIT";
    case ConnectionState::LastAck: return "LAST_ACK";
    case ConnectionState::TimeWait: return "TIME_WAIT";
    case ConnectionState::Failed: return "FAILED";
    }
    return "UNKNOWN";
}

Connection connect_sender(UdpSocket socket, const UdpEndpoint& router, std::uint16_t window,
                          const FileMetadata& metadata, const ConnectionOptions& o) {
    validate_options(o);
    Connection c;
    c.sender = true;
    c.window = window;
    c.metadata = metadata;
    std::random_device random;
    do { c.session_id = static_cast<std::uint32_t>(random()); } while (c.session_id == 0);
    const auto syn = control(c, kFlagSyn);
    state(c, ConnectionState::SynSent, o);
    send(socket, router, c, o, syn);
    RetryTimer timer(o);
    const auto idle = Clock::now() + std::chrono::milliseconds(o.idle_timeout_ms);
    for (;;) {
        if (Clock::now() >= idle) failed(c, o, "handshake made no progress before idle timeout");
        auto packet = matching(socket, router, c, o, std::min(timer.next, idle));
        if (packet && packet->header.flags == (kFlagSyn | kFlagAck)) {
            if (!same_metadata(*packet, c)) {
                abort_connection(socket, router, c, o, "peer changed file parameters");
                throw ConnectionError("peer changed file parameters");
            }
            send(socket, router, c, o, control(c, kFlagAck));
            state(c, ConnectionState::Established, o);
            return c;
        }
        if (Clock::now() >= timer.next) timer.resend(socket, router, c, o, syn);
    }
}

Connection accept_receiver(UdpSocket socket, const UdpEndpoint& router,
                           std::uint16_t expected_window, std::uint16_t expected_payload,
                           const ConnectionOptions& o) {
    validate_options(o);
    if (expected_window == 0 || expected_window > kMaxWindow ||
        expected_payload == 0 || expected_payload > kMaxPayloadBytes)
        throw ConnectionError("invalid receiver window/payload settings");
    Connection c;
    state(c, ConnectionState::Listen, o);
    const auto listen_deadline = Clock::now() + std::chrono::milliseconds(o.idle_timeout_ms);
    for (;;) {
        auto packet = receive_raw(socket, router, o, listen_deadline);
        if (!packet) failed(c, o, "no SYN received before idle timeout");
        if (packet->header.flags != kFlagSyn) continue;
        c.session_id = packet->header.session_id;
        c.window = packet->header.window;
        c.metadata = decode_file_metadata(packet->payload);
        if (c.window != expected_window || c.metadata.payload_bytes != expected_payload) {
            abort_connection(socket, router, c, o, "sender/receiver window or payload mismatch");
            throw ConnectionError("sender/receiver window or payload mismatch");
        }
        break;
    }
    const auto syn_ack = control(c, kFlagSyn | kFlagAck);
    state(c, ConnectionState::SynReceived, o);
    send(socket, router, c, o, syn_ack);
    RetryTimer timer(o);
    const auto idle = Clock::now() + std::chrono::milliseconds(o.idle_timeout_ms);
    for (;;) {
        if (Clock::now() >= idle) failed(c, o, "third handshake step timed out");
        auto packet = matching(socket, router, c, o, std::min(timer.next, idle));
        if (packet) {
            const auto flags = packet->header.flags;
            if (flags == kFlagAck && packet->header.ack == 0) {
                state(c, ConnectionState::Established, o);
                return c;
            }
            if (flags == kFlagData || (flags == kFlagFin && c.metadata.file_bytes == 0)) {
                if (flags == kFlagData && (c.metadata.file_bytes == 0 ||
                    packet->header.seq >= file_packet_count(c.metadata) ||
                    packet->payload.size() > c.metadata.payload_bytes)) continue;
                c.pending_packet = *packet;
                log(o, "[IMPLICIT_HANDSHAKE] preserve first DATA/empty-file FIN");
                state(c, ConnectionState::Established, o);
                return c;
            }
            if (flags == kFlagSyn) {
                if (!same_metadata(*packet, c)) {
                    abort_connection(socket, router, c, o, "duplicate SYN changed metadata");
                    throw ConnectionError("duplicate SYN changed metadata");
                }
                timer.resend(socket, router, c, o, syn_ack);
            }
        }
        if (Clock::now() >= timer.next) timer.resend(socket, router, c, o, syn_ack);
    }
}

std::optional<Packet> receive_connection_packet(UdpSocket socket, const UdpEndpoint& router,
    Connection& c, const ConnectionOptions& o, int timeout_ms) {
    if (timeout_ms < 0) throw ConnectionError("negative connection receive timeout");
    if (c.pending_packet) {
        auto packet = std::move(c.pending_packet);
        c.pending_packet.reset();
        return packet;
    }
    const auto deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);
    for (;;) {
        auto packet = matching(socket, router, c, o, deadline);
        if (!packet) return std::nullopt;
        const auto flags = packet->header.flags;
        if ((c.sender && flags == (kFlagSyn | kFlagAck)) || (!c.sender && flags == kFlagSyn)) {
            if (!same_metadata(*packet, c)) {
                abort_connection(socket, router, c, o, "duplicate handshake changed metadata");
                throw ConnectionError("duplicate handshake changed metadata");
            }
            send(socket, router, c, o, control(c, c.sender ? kFlagAck : kFlagSyn | kFlagAck), true);
            if (Clock::now() >= deadline) return std::nullopt;
            continue;
        }
        return packet;
    }
}

void abort_connection(UdpSocket socket, const UdpEndpoint& router, Connection& c,
                      const ConnectionOptions& o, const std::string& reason) {
    state(c, ConnectionState::Failed, o);
    log(o, "[ABORT] " + reason);
    // Best-effort notification; preserve the original reason if the network fails.
    try { if (c.session_id) send(socket, router, c, o, control(c, kFlagRst)); }
    catch (const std::exception& error) { log(o, std::string("[RST_SEND_FAILED] ") + error.what()); }
}

void close_sender(UdpSocket socket, const UdpEndpoint& router, Connection& c,
                  const ConnectionOptions& o, bool all_data_confirmed) {
    validate_options(o);
    require_established(c, true);
    if (!all_data_confirmed) {
        abort_connection(socket, router, c, o, "unconfirmed data prevents normal close");
        throw ConnectionError("unconfirmed data prevents normal close");
    }
    const auto fin = control(c, kFlagFin);
    state(c, ConnectionState::FinWait1, o);
    send(socket, router, c, o, fin);
    RetryTimer timer(o);
    auto idle = Clock::now() + std::chrono::milliseconds(o.idle_timeout_ms);
    for (;;) {
        if (Clock::now() >= idle) failed(c, o, "waiting for peer FIN timed out");
        const auto wait_until = c.state == ConnectionState::FinWait1 ? std::min(timer.next, idle) : idle;
        auto packet = receive_connection_packet(socket, router, c, o, remaining_ms(wait_until));
        if (packet && packet->header.flags == kFlagAck && packet->header.ack == kCloseAck &&
            c.state == ConnectionState::FinWait1) {
            state(c, ConnectionState::FinWait2, o);
            idle = Clock::now() + std::chrono::milliseconds(o.idle_timeout_ms);
        }
        if (packet && packet->header.flags == kFlagFin) {
            // FIN itself proves the receiver accepted our FIN, even if its ACK was lost.
            send(socket, router, c, o, control(c, kFlagAck, kCloseAck));
            state(c, ConnectionState::TimeWait, o);
            break;
        }
        if (c.state == ConnectionState::FinWait1 && Clock::now() >= timer.next)
            timer.resend(socket, router, c, o, fin);
    }
    // Cover the peer's full FIN retry horizon, including a lost final ACK.
    const auto linger_ms = (static_cast<std::uint64_t>(o.max_retries) + 1) * o.rto_ms;
    const auto linger = Clock::now() + std::chrono::milliseconds(linger_ms);
    log(o, "[TIME_WAIT] duration_ms=" + std::to_string(linger_ms));
    while (Clock::now() < linger) {
        auto packet = receive_connection_packet(socket, router, c, o, remaining_ms(linger));
        if (packet && packet->header.flags == kFlagFin)
            send(socket, router, c, o, control(c, kFlagAck, kCloseAck), true);
    }
    state(c, ConnectionState::Closed, o);
}

void close_receiver(UdpSocket socket, const UdpEndpoint& router, Connection& c,
                    const ConnectionOptions& o, bool file_complete) {
    validate_options(o);
    require_established(c, false);
    if (!file_complete) {
        abort_connection(socket, router, c, o, "incomplete file prevents normal close");
        throw ConnectionError("incomplete file prevents normal close");
    }
    const auto idle = Clock::now() + std::chrono::milliseconds(o.idle_timeout_ms);
    for (;;) {
        auto packet = receive_connection_packet(socket, router, c, o, remaining_ms(idle));
        if (!packet) failed(c, o, "waiting for sender FIN timed out");
        if (packet->header.flags == kFlagFin) break;
    }
    state(c, ConnectionState::CloseWait, o);
    send(socket, router, c, o, control(c, kFlagAck, kCloseAck));
    const auto fin = control(c, kFlagFin);
    send(socket, router, c, o, fin);
    state(c, ConnectionState::LastAck, o);
    RetryTimer timer(o);
    const auto finish_deadline = Clock::now() + std::chrono::milliseconds(o.idle_timeout_ms);
    for (;;) {
        if (Clock::now() >= finish_deadline) failed(c, o, "final close ACK timed out");
        auto packet = receive_connection_packet(socket, router, c, o,
                                                remaining_ms(std::min(timer.next, finish_deadline)));
        if (packet && packet->header.flags == kFlagAck && packet->header.ack == kCloseAck) {
            state(c, ConnectionState::Closed, o);
            return;
        }
        if (packet && packet->header.flags == kFlagFin) {
            send(socket, router, c, o, control(c, kFlagAck, kCloseAck), true);
            timer.resend(socket, router, c, o, fin);
        }
        if (Clock::now() >= timer.next) timer.resend(socket, router, c, o, fin);
    }
}

} // namespace lab3
