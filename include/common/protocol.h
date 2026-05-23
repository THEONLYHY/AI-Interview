#ifndef INCLUDE_COMMON_PROTOCOL_H_
#define INCLUDE_COMMON_PROTOCOL_H_

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "nlohmann/json.hpp"

namespace interview::common {

// 字节序约定：所有多字节整数一律“大端序”，与豆包官方文档一致。
//
// 一条完整消息的线上字节布局（按出现顺序）：
//
//   [Header 4 字节] [Optional 段（按需出现）] [payload_size 4B] [payload]
//
// Header（4 字节，8 个 4bit 字段两两打包）：
//   Byte 0: [version (4bit) | header_size (4bit)]
//           - version       固定 0b0001
//           - header_size   以 4 字节为单位，固定 0b0001（=4 字节）
//   Byte 1: [message_type (4bit) | message_type_specific_flags (4bit)]
//   Byte 2: [serialization (4bit) | compression (4bit)]
//   Byte 3: [reserved (整字节)]
//
// Optional 段（顺序固定，按 message_type 与 flags 决定是否出现）：
//   [code            4B] —— 仅当 message_type == kServerErrorResponse
//   [sequence        4B] —— 仅当 flags 含 kPosSequence / kNegSequence
//   [event           4B] —— 仅当 flags 含 kMsgWithEvent
//   [connect_id_size 4B + connect_id] —— event 为连接级（kConnectionStarted/...）时出现
//   [session_id_size 4B + session_id] —— event 为会话级 / 业务级时出现
//
// 之后才是统一的 [payload_size 4B] [payload]。

// 当前协议版本号
constexpr uint8_t kProtocolVersion = 1;

// 固定头部字节数：官方 4 字节 packed 格式
constexpr std::size_t kProtocolHeaderBytes = 4;

// payload 长度字段字节数：4 字节大端无符号整数
constexpr std::size_t kPayloadSizeBytes = 4;

// MessageType 表示消息类型，占 Byte1 的高 4bit
enum class MessageType : uint8_t {
    kClientFullRequest      = 0b0001,  // 客户端完整请求（JSON）
    kClientAudioOnlyRequest = 0b0010,  // 客户端仅音频请求（PCM 原始字节）
    kServerFullResponse     = 0b1001,  // 服务端完整响应（JSON）
    kServerAck              = 0b1011,  // 服务端 ACK / 音频帧（payload 为二进制）
    kServerErrorResponse    = 0b1111,  // 服务端错误响应（携带 4B code）
};

// MessageFlags 表示消息标志位，占 Byte1 的低 4bit
// 注：豆包文档这一段是“位掩码”而非“互斥枚举”，所以可以按位或组合
enum class MessageFlags : uint8_t {
    kNoSequence   = 0b0000,
    kPosSequence  = 0b0001,  // payload 前带 4B 正向 sequence（原拼写 kProSequence 修正）
    kNegSequence  = 0b0010,  // payload 前带 4B 负向 sequence
    kMsgWithEvent = 0b0100,  // 消息携带 event ID（4B），且按 event 类型继续带 connect_id 或 session_id
};

// SerializationType 表示 payload 的序列化方式，占 Byte2 的高 4bit
enum class SerializationType : uint8_t {
    kNone   = 0b0000,  // 原始字节（音频帧用）
    kJson   = 0b0001,  // JSON 文本（UTF-8）
    kThrift = 0b0011,
    kCustom = 0b1111,
};

// CompressionType 表示 payload 的压缩方式，占 Byte2 的低 4bit
enum class CompressionType : uint8_t {
    kNone   = 0b0000,
    kGzip   = 0b0001,
    kCustom = 0b1111,
};

// 事件 ID 表，对齐豆包官方实时语音对话 API
namespace events {
    // 连接级（客户端发出）
    constexpr uint32_t kStartConnection      = 1;
    constexpr uint32_t kFinishConnection     = 2;
    // 连接级（服务端返回，会带 connect_id）
    constexpr uint32_t kConnectionStarted    = 50;
    constexpr uint32_t kConnectionFailed     = 51;
    constexpr uint32_t kConnectionFinished   = 52;

    // 会话级（客户端发出）
    constexpr uint32_t kStartSession         = 100;
    constexpr uint32_t kFinishSession        = 102;
    // 会话级（服务端返回，会带 session_id）
    constexpr uint32_t kSessionStarted       = 150;
    constexpr uint32_t kSessionFinished      = 152;
    constexpr uint32_t kSessionFailed        = 153;
    constexpr uint32_t kUsage                = 154;
    // 业务级
    constexpr uint32_t kTaskRequest          = 200;  // 客户端送音频
    constexpr uint32_t kChatTextQuery        = 501;  // 文本对话查询，会触发服务端回复与 TTS

    // 服务端 TTS 相关
    constexpr uint32_t kTtsSentenceStart     = 350;
    constexpr uint32_t kTtsSentenceEnd       = 351;
    constexpr uint32_t kTtsResponse          = 352;  // payload 为 PCM 音频（二进制）
    constexpr uint32_t kTtsEnded             = 359;

    // 服务端 ASR 相关
    constexpr uint32_t kAsrInfo              = 450;
    constexpr uint32_t kAsrResult            = 451;
    constexpr uint32_t kAsrEnded             = 459;

    // 服务端 Chat 相关
    constexpr uint32_t kChatResponse         = 550;
    constexpr uint32_t kChatQuestionInfo     = 553;
    constexpr uint32_t kChatEnded            = 559;
}  // namespace events

// ProtocolHeader：协议头的“逻辑结构”（不是网络字节布局）
struct ProtocolHeader {
    // version：占 Byte0 高 4bit
    uint8_t version = kProtocolVersion;
    // header_size：占 Byte0 低 4bit，单位是 4 字节，最小实现固定为 1
    uint8_t header_size = 1;
    // message_type：占 Byte1 高 4bit
    MessageType message_type = MessageType::kClientFullRequest;
    // flags：占 Byte1 低 4bit（位掩码）
    MessageFlags flags = MessageFlags::kNoSequence;
    // serialization：占 Byte2 高 4bit
    SerializationType serialization = SerializationType::kJson;
    // compression：占 Byte2 低 4bit
    CompressionType compression = CompressionType::kNone;
    // reserved：占 Byte3 整字节
    uint8_t reserved = 0;
};

// ProtocolMessage：最简化的“头 + payload”消息体
// 仅用于 Encode/Decode 工具方法（不带 optional 段的简单场景，如自测往返）。
// 真实链路请用 BuildFullRequest / BuildClientAudioRequest / ParseResponse。
struct ProtocolMessage {
    ProtocolHeader header;
    std::vector<uint8_t> payload;
};

// ParsedResponse：服务端帧解析后的统一结构
struct ParsedResponse {
    MessageType message_type = MessageType::kServerFullResponse;
    uint32_t event = 0;                  // event ID（无 kMsgWithEvent flag 时为 0）
    std::string session_id;              // 会话级 / 业务级响应才有
    std::string connect_id;              // 连接级响应才有

    std::string payload_json;            // 文本 payload（已解 gzip）
    std::vector<uint8_t> payload_bytes;  // 二进制 payload（TTS 音频用）
    std::size_t payload_size = 0;        // payload 原始（线上）长度
    int32_t code = 0;                    // 仅 kServerErrorResponse 时有效
    bool is_binary = false;              // payload 是否为二进制（音频）
};

// Protocol：所有编解码静态工具的集合
class Protocol {
public:
    // ---- 比特/字节工具 ----

    // 把 version (高 4bit) 和 header_size (低 4bit) 打包到 Byte0
    static uint8_t BuildByte0(uint8_t version, uint8_t header_size);
    // 从 Byte0 提取 version
    static uint8_t ExtractVersion(uint8_t byte0);
    // 从 Byte0 提取 header_size
    static uint8_t ExtractHeaderSize(uint8_t byte0);

    // 大端序追加 / 读取 32 位无符号整数
    // 旧名 AppendUint32 / ReadUint32 已弃用，统一带 BigEndian 后缀避免歧义
    static void AppendUint32BigEndian(std::vector<uint8_t>& buffer,
                                      uint32_t value);
    static uint32_t ReadUint32BigEndian(const std::vector<uint8_t>& buffer,
                                        std::size_t offset);

    // ---- 简单编解码（仅头 + payload，无 optional 段） ----

    static std::vector<uint8_t> Encode(const ProtocolMessage& message);
    // Decode 失败抛 std::runtime_error
    static ProtocolMessage Decode(const std::vector<uint8_t>& data);

    // ---- 真实业务侧编解码 ----

    // 构造客户端 Full Request 帧（JSON payload）
    // - msg_type=kClientFullRequest, flags=kMsgWithEvent, ser=kJson, compression=kNone
    // - session_id 为空时不写入会话段（适用于 kStartConnection 等连接级事件）
    static std::vector<uint8_t> BuildFullRequest(uint32_t event,
                                                 const std::string& session_id,
                                                 const nlohmann::json& payload);

    // 构造 ChatTextQuery payload。该事件不是纯 TTS 接口，content 必须
    // 包成明确的朗读指令，避免服务端把面试题当成候选人的技术提问回答。
    static nlohmann::json BuildReadAloudTextQueryPayload(
        const std::string& text);

    // 构造客户端 Audio Only 帧（PCM 原始字节）
    // - msg_type=kClientAudioOnlyRequest, flags=kMsgWithEvent, ser=kNone, compression=kNone
    // - 通常 event=events::kTaskRequest(=200)
    static std::vector<uint8_t> BuildClientAudioRequest(
        uint32_t event,
        const std::string& session_id,
        const std::vector<uint8_t>& pcm);

    // 解析服务端帧（含全部 optional 段处理 + gzip 自动解压）
    // 解析失败抛 std::runtime_error；上层捕获后做错误回调即可
    static ParsedResponse ParseResponse(const std::vector<uint8_t>& data);

    // ---- gzip 工具 ----
    // 服务端可能对大 payload 用 gzip，需要双向支持
    // 解压失败抛 std::runtime_error
    static std::vector<uint8_t> CompressGzip(const std::vector<uint8_t>& src);
    static std::vector<uint8_t> DecompressGzip(const std::vector<uint8_t>& src);
};

}  // namespace interview::common

#endif
