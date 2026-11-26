#ifndef LAB3_UDP_IO_H
#define LAB3_UDP_IO_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
// Winsock2 must precede windows.h to avoid the old Winsock declarations.
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#endif

namespace lab3 {

#ifdef _WIN32
using UdpSocket = SOCKET;
inline constexpr UdpSocket kInvalidUdpSocket = INVALID_SOCKET;
#else
using UdpSocket = int;
inline constexpr UdpSocket kInvalidUdpSocket = -1;
#endif
//保存一个 IP 和端口
struct UdpEndpoint {
    std::string ip;
    std::uint16_t port = 0;
};

enum class UdpReceiveStatus { Received, Timeout, Oversized };

struct UdpReceiveResult {
    UdpReceiveStatus status = UdpReceiveStatus::Timeout;
    UdpEndpoint source;
    std::vector<std::uint8_t> data;
};

//初始化网络环境
void udp_initialize();
//清理网络环境
void udp_cleanup() noexcept;
//创建并绑定 UDP Socket
UdpSocket udp_open_bound(const UdpEndpoint& local);
//关闭 Socket
void udp_close(UdpSocket& socket) noexcept;
//查询实际绑定的本地地址
UdpEndpoint udp_local_endpoint(UdpSocket socket);
//发送一个 UDP 数据报
void udp_send(UdpSocket socket, const UdpEndpoint& destination,
              const std::vector<std::uint8_t>& data);
//等待并接收一个 UDP 数据报
UdpReceiveResult udp_receive(UdpSocket socket, int timeout_ms);
} 

#endif // LAB3_UDP_IO_H
