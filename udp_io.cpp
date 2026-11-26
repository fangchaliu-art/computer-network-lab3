#include "udp_io.h"
#include "protocol.h"

#include <array>
#include <chrono>
#include <stdexcept>
#include <system_error>

#ifndef _WIN32
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#endif

namespace lab3 {
namespace {

using Clock = std::chrono::steady_clock;
#ifdef _WIN32
using AddressLength = int;
#else
using AddressLength = socklen_t;
#endif

int socket_error() noexcept {
#ifdef _WIN32
    return WSAGetLastError();
#else
    return errno;
#endif
}

// Keep diagnostics ASCII/UTF-8 safe: Windows system_category messages can use
// the ANSI code page while the application's console output uses UTF-8.
class SocketErrorCategory final : public std::error_category {
public:
    const char* name() const noexcept override { return "native_socket"; }
    std::string message(int code) const override {
        return "native socket error " + std::to_string(code);
    }
};

[[noreturn]] void fail(const char* operation, int code) {
    static const SocketErrorCategory category;
    throw std::system_error(code, category, operation);
}

bool interrupted(int code) noexcept {
#ifdef _WIN32
    return code == WSAEINTR;
#else
    return code == EINTR;
#endif
}

bool would_block(int code) noexcept {
#ifdef _WIN32
    return code == WSAEWOULDBLOCK;
#else
    return code == EAGAIN || code == EWOULDBLOCK;
#endif
}

void require_socket(UdpSocket socket) {
    if (socket == kInvalidUdpSocket)
        throw std::invalid_argument("invalid UDP socket handle");
}

//地址转换：让我们定义的地址能交给系统使用
sockaddr_in native_endpoint(const UdpEndpoint& endpoint) {
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(endpoint.port);
    if (inet_pton(AF_INET, endpoint.ip.c_str(), &address.sin_addr) != 1)
        throw std::invalid_argument("invalid numeric IPv4 endpoint: " + endpoint.ip);
    return address;
}
//反向转换
UdpEndpoint public_endpoint(const sockaddr_in& address) {
    std::array<char, INET_ADDRSTRLEN> ip{};
    if (!inet_ntop(AF_INET, &address.sin_addr, ip.data(),
                   static_cast<AddressLength>(ip.size())))
        fail("inet_ntop", socket_error());
    return {ip.data(), ntohs(address.sin_port)};
}

} 

//初始化 Windows 网络接口
void udp_initialize() {
#ifdef _WIN32
    WSADATA data{};
    //完成初始化，并将相关信息写入 data。MAKEWORD(2, 2)表示请求 Winsock 2.2 版本。
    const int code = WSAStartup(MAKEWORD(2, 2), &data);
    // WSAStartup() 失败时直接返回错误码
    if (code != 0) fail("WSAStartup", code);
    //检查初始化后获得的 Winsock 版本是否为程序要求的 2.2
    if (data.wVersion != MAKEWORD(2, 2)) {
        WSACleanup();
        throw std::runtime_error("Winsock 2.2 is unavailable");
    }
#endif
}

void udp_cleanup() noexcept {
#ifdef _WIN32
    WSACleanup();
#endif
}

void udp_close(UdpSocket& socket) noexcept {
    //已经无效，就直接结束
    if (socket == kInvalidUdpSocket) return;
#ifdef _WIN32
    closesocket(socket);
#else
    close(socket);
#endif
    socket = kInvalidUdpSocket;
}
//创建 Socket 并绑定本地地址
UdpSocket udp_open_bound(const UdpEndpoint& local) {
    const auto address = native_endpoint(local);
    UdpSocket handle = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (handle == kInvalidUdpSocket) fail("socket", socket_error());
    try {
#ifdef _WIN32
        //避免其他程序共享同一个本地端点，使端口冲突能够明确报错 
        const BOOL exclusive = TRUE;
        if (setsockopt(handle, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
                       reinterpret_cast<const char*>(&exclusive), sizeof(exclusive)) != 0)
            fail("setsockopt(SO_EXCLUSIVEADDRUSE)", socket_error());
        //设置非阻塞
        u_long nonblocking = 1;
        if (ioctlsocket(handle, FIONBIO, &nonblocking) != 0)
            fail("ioctlsocket(FIONBIO)", socket_error());
#else
        const int flags = fcntl(handle, F_GETFL, 0);
        if (flags == -1 || fcntl(handle, F_SETFL, flags | O_NONBLOCK) == -1)
            fail("fcntl(O_NONBLOCK)", socket_error());
#endif
        //让这个 Socket 使用指定的本地 IP 和端口
        if (::bind(handle, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0)
            fail("bind", socket_error());
    } catch (...) {
        udp_close(handle);
        throw;
    }
    return handle;
}
//查询实际绑定结果
UdpEndpoint udp_local_endpoint(UdpSocket socket) {
    require_socket(socket);
    sockaddr_in address{};
    AddressLength length = sizeof(address);
    if (getsockname(socket, reinterpret_cast<sockaddr*>(&address), &length) != 0)
        fail("getsockname", socket_error());
    return public_endpoint(address);
}
//实际发送一个数据报
void udp_send(UdpSocket socket, const UdpEndpoint& destination,
              const std::vector<std::uint8_t>& data) {
    //先检查确认句柄不是我们定义的无效值
    require_socket(socket);
    //检查报文长度和目标端口
    if (data.size() > kRouterMaxPacketBytes)
        throw std::invalid_argument("UDP datagram exceeds router limit of 15000 bytes");
    if (destination.port == 0)
        throw std::invalid_argument("UDP destination port must not be zero");
    const auto address = native_endpoint(destination);
    const char* buffer = data.empty() ? "" : reinterpret_cast<const char*>(data.data());
    for (;;) {
        //真正发送
        const auto sent = sendto(socket, buffer, static_cast<int>(data.size()), 0,
                                 reinterpret_cast<const sockaddr*>(&address), sizeof(address));
        //检查返回值
        if (sent < 0) {
            const int code = socket_error();
            if (interrupted(code)) continue;
            fail("sendto", code);
        }
        //如果只是系统调用被中断，就重新调用；其他错误抛给上层
        if (static_cast<std::size_t>(sent) != data.size())
            throw std::runtime_error("sendto did not accept the complete UDP datagram");
        return;
    }
}

//先等数据，再读取数据
UdpReceiveResult udp_receive(UdpSocket socket, int timeout_ms) {
    require_socket(socket);
    if (timeout_ms < 0) throw std::invalid_argument("UDP timeout must not be negative");
#ifndef _WIN32
    if (socket >= FD_SETSIZE)
        throw std::invalid_argument("UDP socket exceeds select FD_SETSIZE");
#endif
    const auto deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);
    std::array<char, kRouterMaxPacketBytes + 1> buffer{};
    for (;;) {
        const auto now = Clock::now();
        //每轮重新计算，而不是每次重新等待完整的 1000ms。
        const auto remaining = now < deadline
            ? std::chrono::duration_cast<std::chrono::microseconds>(deadline - now).count()
            : 0;
        timeval timeout{};
        //超时参数由“秒”和“微秒”两部分组成
        timeout.tv_sec = static_cast<long>(remaining / 1000000);
        timeout.tv_usec = static_cast<long>(remaining % 1000000);
        fd_set readable;
        //清空要监视的集合
        FD_ZERO(&readable);
        //把当前Socket放进去
        FD_SET(socket, &readable);
#ifdef _WIN32
        const int ready = select(0, &readable, nullptr, nullptr, &timeout);
#else
        const int ready = select(socket + 1, &readable, nullptr, nullptr, &timeout);
#endif
        //小于 0系统调用错误
        if (ready < 0) {
            const int code = socket_error();
            if (interrupted(code) && Clock::now() < deadline) continue;
            if (interrupted(code)) return {};
            fail("select", code);
        }
        //等于0等待超时
        if (ready == 0) return {};

        sockaddr_in source{};
        AddressLength source_length = sizeof(source);
        //拿到报文内容和来源
        const auto received = recvfrom(socket, buffer.data(), static_cast<int>(buffer.size()), 0,
                                       reinterpret_cast<sockaddr*>(&source), &source_length);
        if (received < 0) {
            const int code = socket_error();
#ifdef _WIN32
            if (code == WSAEMSGSIZE)
                return {UdpReceiveStatus::Oversized, {}, {}};
#endif
            if (interrupted(code) || would_block(code)) {
                if (Clock::now() >= deadline) return {};
                continue;
            }
            fail("recvfrom", code);
        }
        if (static_cast<std::size_t>(received) > kRouterMaxPacketBytes)
            return {UdpReceiveStatus::Oversized, {}, {}};
        //将收到的数据整理成返回结果
        UdpReceiveResult result;
        result.status = UdpReceiveStatus::Received;
        result.source = public_endpoint(source);
        const auto* bytes = reinterpret_cast<const std::uint8_t*>(buffer.data());
        result.data.assign(bytes, bytes + received);
        return result;
    }
}

} // namespace lab3
