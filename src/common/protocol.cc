#include "common/protocol.h"

#include <stdexcept>

// 将version 和 header_size 打包到第1个字节中
// 高4 bit 存version， 低 4 bit 存 header_size
uint8_t Protocol::BuildByte0(uint8_t version, uint8_t header_size) {
    // 只允许保留低4位
    uint8_t high = static_cast<uint8_t>((version & 0x0F) << 4);
    uint8_t low = static_cast<uint8_t>(header_size & 0x0F);
    return static_cast<uint8_t>(high | low);
}

// 从第一个字节中提取 version
// versino 位于高 4 bit
uint8_t Protocol::ExtractVersion(uint8_t byte0) {
    return static_cast<uint8_t>((byte0 >> 4) & 0x0F);
}

// 从第一个字节中提取 header_size
uint8_t Protocol::ExtractHeaderSize(uint8_t byte0) {
    return static_cast<uint8_t>(byte0 & 0x0F);
}
// payload_size的4个字节
// 按大端序 将32位无符号整数追加到buffer 末尾
// 例如 value = 0x12345678, 会写入4个字节:
// 0x12 0x34 0x56 0x78
void Protocol::AppendUint32(std::vector<uint8_t>& buffer, 
                                uint32_t value) {
    buffer.push_back(static_cast<uint8_t>((value >> 24) & 0xFF));
    buffer.push_back(static_cast<uint8_t>((value >> 16) & 0xFF));
    buffer.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
    buffer.push_back(static_cast<uint8_t>(value & 0xFF));
}

// 按大端序 从buffer的offset的指定位置读取32为无符号整形，
// 如果剩余字节不足4个，则抛出异常
uint32_t Protocol::ReadUint32(const std::vector<uint8_t>& buffer,
                                std::size_t offset) {
    if (offset + kPayloadSizeBytes > buffer.size()) {
        throw std::out_of_range("buffer too small to read uint32");
    }

    uint32_t value = 0;
    value |= static_cast<uint32_t>(buffer[offset]) << 24;
    value |= static_cast<uint32_t>(buffer[offset + 1]) << 16;
    value |= static_cast<uint32_t>(buffer[offset + 2]) << 8;
    value |= static_cast<uint32_t>(buffer[offset + 3]);

    return value;
}

// 将结构化消息编码成字节流
std::vector<uint8_t> Protocol::Encode(const ProtocolMessage& message) {
    std::vector<uint8_t> bytes;
    bytes.reserve(kProtocolHeaderBytes + kPayloadSizeBytes + message.payload.size());
    // 第 1 个字节： verion + header_size
    bytes.push_back(BuildByte0(message.header.version, message.header.header_size));
    // 第 2 个字节： message_type
    bytes.push_back(static_cast<uint8_t>(message.header.message_type));
    // 第 3 个字节： flags
    bytes.push_back(message.header.flags);
    // 第 4 个字节：serialization
    bytes.push_back(static_cast<uint8_t>(message.header.serialization));
    // payload 长度字段，4字节大端序
    AppendUint32(bytes, static_cast<uint32_t>(message.payload.size()));
    // 追加 payload 数据
    bytes.insert(bytes.end(), message.payload.begin(), message.payload.end());

    return bytes;
}

// 将字节流解码或结构化消息
// 如果数据非法，抛出异常
ProtocolMessage Protocol::Decode(const std::vector<uint8_t>& data) {
    // 至少需要4字节固定头 + 4字节payload 长度
    if (data.size() < kProtocolHeaderBytes + kPayloadSizeBytes) {
        throw std::runtime_error("protocol data too short");
    }

    ProtocolMessage message;

    //
    uint8_t byte0 = data[0];
    message.header.version = ExtractVersion(byte0);
    message.header.header_size = ExtractHeaderSize(byte0);
    message.header.message_type = static_cast<MessageType>(data[1]);
    message.header.flags = data[2];
    message.header.serialization = static_cast<SerializationType>(data[3]);

    if (message.header.header_size != 1) {
        throw std::runtime_error("unsupported header_size in minimal protocol");
    }
    // 从data的第kProtocolHeaderBytes个字节位置开始，读取一个4字节的uint32_t
    // 读取payload长度
    uint32_t payload_size = ReadUint32(data, kProtocolHeaderBytes);

    // 校验整体长度是否合法
    std::size_t excepted_size = kProtocolHeaderBytes + kPayloadSizeBytes + payload_size;
    if (data.size() < excepted_size) {
        throw std::runtime_error("protocol payload size mismatch");
    }
    // 提取payload
    std::size_t payload_offset = kProtocolHeaderBytes + kPayloadSizeBytes;
    message.payload.assign(data.begin() + payload_offset,
                            data.begin() + payload_offset + payload_size);
    return message;
}