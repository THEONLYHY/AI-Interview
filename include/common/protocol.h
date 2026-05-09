#ifndef INCLUDE_COMMON_PROTOCOL_H_
#define INCLUDE_COMMON_PROTOCOL_H_

#include <cstddef>
#include <cstdint>
#include <vector>

//[协议头 4字节] [可选字段] [payload大小 4字节] [payload数据]
// 协议头布局
// Byte 0: [version(4bit)] | header_size(4bit)
// Byte 1: message_type
// Byte 2: flags
// Byte 3: serialization

// 当前协议版本号
constexpr uint8_t kProtocolVersion = 1;

// 固定头部字节数
// 当前最小实现只使用4字节固定头
constexpr std::size_t kProtocolHeaderBytes = 4;

// payload长度字段字节数
// 当前使用4字节无符号整数表示payload 大小
constexpr std::size_t kPayloadSizeBytes = 4;

// MessageType 表示消息类型
// 第四阶段先保留最小合集，后面再逐步扩展
enum class MessageType : uint8_t {
    kUnkown = 0,
    kClientEvent = 1, // 客户端事件
    kServerEvent = 2, // 服务端事件
    kAuidoData = 3,   // 音频数据
    kError = 15,      // 错误消息
};

// SerializationType 表示payload 的序列化方式
enum class SerializationType : uint8_t {
    kNone = 0,
    kJson = 1,  // JSON文本
    kBinary = 2, // 二进制音频数据
};

// ProtoclHeader 表示协议头的逻辑字段
// 注意： 这是“解析后的逻辑结构”， 不是网络上传输时的原始内存布局
struct ProtocolHeader {
    // 协议版本，占第1个字节的高 4bit
    uint8_t version = kProtocolVersion;
    // 头部长度，占第1个字节的低 4bit
    // 当前最小实现固定为 1， 表示1个 4-byte header unit
    uint8_t header_size = 1;

    // 消息类型，占第2个字节
    MessageType message_type = MessageType::kUnkown;

    // 标志位， 占第3个字节
    // 默认填0
    uint8_t flags = 0;

    // 序列化方式，占第 4 个字节
    SerializationType serialization = SerializationType::kNone;
};

// ProtocolMessage 表示一条完整的协议消息
// 当前最小实现只含： 协议头 + payload
struct ProtocolMessage {
    // 协议头
    ProtocolHeader header;

    // 真正的payload数据
    std::vector<uint8_t> payload;
};


// 最小协议格式：
// [4字节固定头] [4字节payload长度][payload]
class Protocol {
public:
    // 将version 和 header_size 打包到第1个字节中
    // 高4 bit 存version， 低 4 bit 存 header_size
    static uint8_t BuildByte0(uint8_t version, uint8_t header_size);
    // 从第一个字节中提取 version
    static uint8_t ExtractVersion(uint8_t byte0);
    // 从第一个字节中提取 header_size
    static uint8_t ExtractHeaderSize(uint8_t byte0);
    // 按大端序 将32为无符号整数追加到buffer 末尾
    // 
    static void AppendUint32(std::vector<uint8_t>& buffer, uint32_t value);
    
    static uint32_t ReadUint32(const std::vector<uint8_t>& buffer,
                                std::size_t offset);
    // 将结构化消息编码成字节流
    static std::vector<uint8_t> Encode(const ProtocolMessage& message);
    // 将字节流解码或结构化消息
    // 如果数据非法，抛出异常
    static ProtocolMessage Decode(const std::vector<uint8_t>& data);
};





#endif