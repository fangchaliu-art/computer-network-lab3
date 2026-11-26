#include "protocol.h"

#include <limits>

//发送：Packet对象 → 检查字段 → 编码首部和载荷 → 计算校验和 → 字节数组
//接收：字节数组 → 检查长度和校验和 → 还原字段 → 检查字段 → Packet对象
namespace lab3 {
namespace {
//把整数写成网络字节序
void put16(std::vector<std::uint8_t>& out, std::size_t offset, std::uint16_t value) {
    out[offset] = static_cast<std::uint8_t>(value >> 8);
    out[offset + 1] = static_cast<std::uint8_t>(value);
}

void put32(std::vector<std::uint8_t>& out, std::size_t offset, std::uint32_t value) {
    for (std::size_t i = 0; i < 4; ++i)
        out[offset + i] = static_cast<std::uint8_t>(value >> (24 - 8 * i));
}

std::uint16_t get16(const std::vector<std::uint8_t>& in, std::size_t offset) {
    return static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(in[offset]) << 8) | in[offset + 1]);
}

std::uint32_t get32(const std::vector<std::uint8_t>& in, std::size_t offset) {
    std::uint32_t value = 0;
    for (std::size_t i = 0; i < 4; ++i) value = (value << 8) | in[offset + i];
    return value;
}
//识别报文类型
const char* flag_name(std::uint8_t flags) {
    switch (flags) {
    case kFlagSyn: return "SYN";
    case kFlagSyn | kFlagAck: return "SYN|ACK";
    case kFlagAck: return "ACK";
    case kFlagFin: return "FIN";
    case kFlagData: return "DATA";
    case kFlagRst: return "RST";
    default: throw ProtocolError("unsupported packet flags");
    }
}
//检查文件元信息
void validate_metadata(const FileMetadata& metadata) {
    //检查数据块大小
    if (metadata.payload_bytes == 0 || metadata.payload_bytes > kMaxPayloadBytes)
        throw ProtocolError("metadata payload size must be in [1, 14976]");
    //计算需要多少个数据包
    const auto count = metadata.file_bytes / metadata.payload_bytes +
                       (metadata.file_bytes % metadata.payload_bytes != 0);
    //检查包数量是否超出当前设计的限制
    if (count > std::numeric_limits<std::uint32_t>::max())
        throw ProtocolError("file needs too many packets for a non-wrapping session");
}
//检查一个逻辑报文是否符合约定
void validate_packet(const Packet& packet) {
    const auto& h = packet.header;
    (void)flag_name(h.flags);
    if (h.session_id == 0) throw ProtocolError("session ID must not be zero");
    if (h.window == 0 || h.window > kMaxWindow)
        throw ProtocolError("window size must be in [1, 1024]");
    if (packet.payload.size() > kMaxPayloadBytes)
        throw ProtocolError("packet exceeds router datagram size limit");
    if (h.flags != kFlagData && h.seq != 0)
        throw ProtocolError("control packet seq must be zero");
    if (h.flags != kFlagAck && h.ack != 0)
        throw ProtocolError("ack field is only used by pure ACK packets");

    if (h.flags == kFlagSyn || h.flags == (kFlagSyn | kFlagAck)) {
        (void)decode_file_metadata(packet.payload);
    } else if (h.flags == kFlagData) {
        if (packet.payload.empty()) throw ProtocolError("DATA must contain file bytes");
    } else if (!packet.payload.empty()) {
        throw ProtocolError("ACK/FIN/RST must not contain a payload");
    }
}

} // namespace
//计算 16 位反码校验和
std::uint16_t compute_checksum(const std::vector<std::uint8_t>& bytes) noexcept {
    std::uint32_t sum = 0;
    for (std::size_t i = 0; i < bytes.size(); i += 2) {
        std::uint32_t word = static_cast<std::uint32_t>(bytes[i]) << 8;
        if (i + 1 < bytes.size()) word |= bytes[i + 1];
        sum += word;
        // Fold on every addition so the accumulator cannot overflow on long input.
        sum = (sum & 0xFFFFu) + (sum >> 16);
    }
    while (sum >> 16) sum = (sum & 0xFFFFu) + (sum >> 16);
    return static_cast<std::uint16_t>(~sum);
}
//文件元信息怎样编码和解码
std::vector<std::uint8_t> encode_file_metadata(const FileMetadata& metadata) {
    validate_metadata(metadata);
    std::vector<std::uint8_t> bytes(kFileMetadataBytes, 0);
    put32(bytes, 0, static_cast<std::uint32_t>(metadata.file_bytes >> 32));
    put32(bytes, 4, static_cast<std::uint32_t>(metadata.file_bytes));
    put16(bytes, 8, metadata.payload_bytes);
    return bytes;
}

FileMetadata decode_file_metadata(const std::vector<std::uint8_t>& bytes) {
    if (bytes.size() != kFileMetadataBytes)
        throw ProtocolError("SYN metadata must be exactly 10 bytes");
    FileMetadata metadata;
    metadata.file_bytes = (static_cast<std::uint64_t>(get32(bytes, 0)) << 32) |
                          get32(bytes, 4);
    metadata.payload_bytes = get16(bytes, 8);
    validate_metadata(metadata);
    return metadata;
}

std::uint64_t file_packet_count(const FileMetadata& metadata) {
    validate_metadata(metadata);
    return metadata.file_bytes / metadata.payload_bytes +
           (metadata.file_bytes % metadata.payload_bytes != 0);
}
//生成真正要发送的字节数组
std::vector<std::uint8_t> encode_packet(const Packet& packet) {
    validate_packet(packet);
    //创建 24 个零字节，作为首部空间
    std::vector<std::uint8_t> bytes(kHeaderBytes, 0);
    put16(bytes, 0, kProtocolMagic);
    bytes[2] = kProtocolVersion;
    bytes[3] = packet.header.flags;
    put32(bytes, 4, packet.header.session_id);
    put32(bytes, 8, packet.header.seq);
    put32(bytes, 12, packet.header.ack);
    put16(bytes, 16, packet.header.window);
    put16(bytes, 18, static_cast<std::uint16_t>(packet.payload.size()));
    //追加载荷
    bytes.insert(bytes.end(), packet.payload.begin(), packet.payload.end());
    put16(bytes, 20, compute_checksum(bytes));
    return bytes;
}

Packet decode_packet(const std::vector<std::uint8_t>& bytes) {
    // Check bounds before reading any fixed offsets or trusting the length field.
    if (bytes.size() < kHeaderBytes) throw ProtocolError("truncated protocol header");
    if (bytes.size() > kRouterMaxPacketBytes) throw ProtocolError("oversized packet");
    if (bytes.size() != kHeaderBytes + get16(bytes, 18))
        throw ProtocolError("payload length does not match datagram length");
    if (compute_checksum(bytes) != 0) throw ProtocolError("checksum mismatch");
    if (get16(bytes, 0) != kProtocolMagic) throw ProtocolError("wrong protocol magic");
    if (bytes[2] != kProtocolVersion) throw ProtocolError("unsupported protocol version");
    if (get16(bytes, 22) != 0) throw ProtocolError("reserved field must be zero");

    Packet packet;
    packet.header.flags = bytes[3];
    packet.header.session_id = get32(bytes, 4);
    packet.header.seq = get32(bytes, 8);
    packet.header.ack = get32(bytes, 12);
    packet.header.window = get16(bytes, 16);
    packet.payload.assign(bytes.begin() + kHeaderBytes, bytes.end());
    validate_packet(packet);
    return packet;
}

std::string packet_summary(const Packet& packet) {
    return std::string(flag_name(packet.header.flags)) +
           " session=" + std::to_string(packet.header.session_id) +
           " seq=" + std::to_string(packet.header.seq) +
           " ack=" + std::to_string(packet.header.ack) +
           " window=" + std::to_string(packet.header.window) +
           " length=" + std::to_string(packet.payload.size());
}

} // namespace lab3
