#ifndef LAB3_PROTOCOL_H
#define LAB3_PROTOCOL_H

#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace lab3 {

// 协议大小等常量
inline constexpr std::uint32_t kHeaderBytes = 24;
inline constexpr std::uint32_t kRouterMaxPacketBytes = 15000;
inline constexpr std::uint32_t kMaxPayloadBytes = kRouterMaxPacketBytes - kHeaderBytes;
//允许配置的窗口上限
inline constexpr std::uint32_t kMaxWindow = 1024;
//协议标识、版本和元信息长度
inline constexpr std::uint16_t kProtocolMagic = 0x4C33; // ASCII "L3"
inline constexpr std::uint8_t kProtocolVersion = 1;
inline constexpr std::uint32_t kFileMetadataBytes = 10;

//请求建立连接
inline constexpr std::uint8_t kFlagSyn = 0x01;
//确认
inline constexpr std::uint8_t kFlagAck = 0x02;
//请求断开连接
inline constexpr std::uint8_t kFlagFin = 0x04;
//携带文件数据
inline constexpr std::uint8_t kFlagData = 0x08;
//异常终止连接
inline constexpr std::uint8_t kFlagRst = 0x10;

//程序中保存的首部信息
struct PacketHeader {
    std::uint8_t flags = kFlagData;
    //会话标识
    std::uint32_t session_id = 0; 
    //数据块序号
    std::uint32_t seq = 0;
    //确认哪个数据块
    // 连接管理使用UINT32_MAX确认FIN；该值不会用作有效数据块序号。
    std::uint32_t ack = 0;
    std::uint16_t window = 16;
};

struct Packet {
    PacketHeader header;
    std::vector<std::uint8_t> payload;
};

struct FileMetadata {
    std::uint64_t file_bytes = 0;
    std::uint16_t payload_bytes = 1200;
};

class ProtocolError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// 校验和函数声明
std::uint16_t compute_checksum(const std::vector<std::uint8_t>& bytes) noexcept;

// 报文编码和解码接口
//Packet对象 → 可以发送的字节数组
std::vector<std::uint8_t> encode_packet(const Packet& packet);
//收到的字节数组 → Packet对象
Packet decode_packet(const std::vector<std::uint8_t>& bytes);

//负责文件元信息与 10 字节载荷之间的转换
std::vector<std::uint8_t> encode_file_metadata(const FileMetadata& metadata);
FileMetadata decode_file_metadata(const std::vector<std::uint8_t>& bytes);
//根据文件长度和块大小计算需要多少个数据包
std::uint64_t file_packet_count(const FileMetadata& metadata);

// 生成便于打印的摘要
std::string packet_summary(const Packet& packet);

} // namespace lab3

#endif // LAB3_PROTOCOL_H
