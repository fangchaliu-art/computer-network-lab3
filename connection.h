#ifndef LAB3_CONNECTION_H
#define LAB3_CONNECTION_H

#include "protocol.h"
#include "udp_io.h"

#include <limits>
#include <optional>
#include <ostream>

namespace lab3 {
//当前连接走到了哪一步
enum class ConnectionState {
    Closed, Listen, SynSent, SynReceived, Established,
    FinWait1, FinWait2, CloseWait, LastAck, TimeWait, Failed
};
//连接过程的运行参数
struct ConnectionOptions {
    std::uint32_t rto_ms = 1000;
    std::uint32_t max_retries = 20;
    std::uint32_t idle_timeout_ms = 120000;
    std::ostream* log = nullptr; // Optional log stream owned by caller.
};

//专门表示关闭确认
inline constexpr std::uint32_t kCloseAck = std::numeric_limits<std::uint32_t>::max();
//保存这一次连接的当前情况
struct Connection {
    ConnectionState state = ConnectionState::Closed;
    bool sender = false;
    std::uint32_t session_id = 0;
    std::uint16_t window = 16;
    FileMetadata metadata;
    std::optional<Packet> pending_packet;
    std::uint64_t control_sent = 0;
    std::uint64_t retransmissions = 0;
};

class ConnectionError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};
//将状态变成日志文字
const char* connection_state_name(ConnectionState state) noexcept;


Connection connect_sender(UdpSocket socket, const UdpEndpoint& router,
                          std::uint16_t window, const FileMetadata& metadata,
                          const ConnectionOptions& options);
Connection accept_receiver(UdpSocket socket, const UdpEndpoint& router,
                           std::uint16_t expected_window, std::uint16_t expected_payload,
                           const ConnectionOptions& options);

//带连接检查的收包入口
std::optional<Packet> receive_connection_packet(
    UdpSocket socket, const UdpEndpoint& router, Connection& connection,
    const ConnectionOptions& options, int timeout_ms);

// Caller MUST establish file completion first. false sends RST and fails.
void close_sender(UdpSocket socket, const UdpEndpoint& router, Connection& connection,
                  const ConnectionOptions& options, bool all_data_confirmed);
void close_receiver(UdpSocket socket, const UdpEndpoint& router, Connection& connection,
                    const ConnectionOptions& options, bool file_complete);
void abort_connection(UdpSocket socket, const UdpEndpoint& router, Connection& connection,
                      const ConnectionOptions& options, const std::string& reason);

} // namespace lab3

#endif // LAB3_CONNECTION_H
