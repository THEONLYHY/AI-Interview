#include "common/protocol.h"

#include <cstring>
#include <stdexcept>
#include <string>
#include <utility>
#include <zlib.h>

namespace interview::common {

namespace {
bool IsConnectionLevelEvent(uint32_t event) {
    switch (event) {
    case events::kConnectionStarted:
    case events::kConnectionFailed:
    case events::kConnectionFinished:
        return true;
    default:
        return false;
    }
}
// 取低4bit，用于打包4bit字段
inline uint8_t Low4(uint8_t v) {
    return static_cast<uint8_t>(v & 0x0F);
}

// 把任意enum class转成 uint8_t
template <typename E>
inline uint8_t U8(E e) {
    return static_cast<uint8_t>(e);
}

// 检测flags 位掩码是否包含某一位
inline bool HasFlag(MessageFlags flags, MessageFlags bit) {
    return (U8(flags) & U8(bit)) != 0;
}

// 把字节序列 emplace 到 buffer 尾部
inline void AppendBytes(std::vector<uint8_t>& buffer,
                        const uint8_t* data, std::size_t size) {
    buffer.insert(buffer.end(), data, data + size);
}

// 把 std::string 当作 UTF-8 字节序列追加
inline void AppendStringBytes(std::vector<uint8_t>& buffer,
                                const std::string& s) {
    buffer.insert(buffer.end(), s.begin(), s.end());
}

// 写入“[size 4B] + [bytes]”格式的可变长字段
inline void AppendSizedString(std::vector<uint8_t>& buffer,
                              const std::string& s) {
    Protocol::AppendUint32BigEndian(buffer, static_cast<uint32_t>(s.size()));
    AppendStringBytes(buffer, s);
}

// 从 data[offset:] 读“[size 4B] + [bytes]”，返回 string 并把 offset 推进
std::string ReadSizedString(const std::vector<uint8_t>& data,
                            std::size_t& offset) {
    uint32_t size = Protocol::ReadUint32BigEndian(data, offset);
    offset += kPayloadSizeBytes;
    if (offset + size > data.size()) {
        throw std::runtime_error("protocol size-string out of range");
    }
    std::string s(data.begin() + offset, data.begin() + offset + size);
    offset += size;
    return s;
}

// 构造一条客户端请求帧的 4 字节 packed 头
// 抽出来给 BuildFullRequest / BuildClientAudioRequest 共用，避免散装比特运算
void AppendPackedHeader(std::vector<uint8_t>& buffer,
                        MessageType msg_type,
                        MessageFlags flags,
                        SerializationType ser,
                        CompressionType comp) {
    buffer.push_back(Protocol::BuildByte0(kProtocolVersion, 1));
    buffer.push_back(static_cast<uint8_t>((Low4(U8(msg_type)) << 4) | Low4(U8(flags))));
    buffer.push_back(static_cast<uint8_t>((Low4(U8(ser)) << 4) | Low4(U8(comp))));
    buffer.push_back(0); //reserved
}
}

// 将version 和 header_size 打包到第1个字节中
// 高4 bit 存version， 低 4 bit 存 header_size
uint8_t Protocol::BuildByte0(uint8_t version, uint8_t header_size) {
    // 只允许保留低4位
    // high = 0001 << 4 = 00010000
    // low = 00000001
    // OR之后 00010001 = 0x11 = [0001 | 0001]
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
void Protocol::AppendUint32BigEndian(std::vector<uint8_t>& buffer,
                                uint32_t value) {
    buffer.push_back(static_cast<uint8_t>((value >> 24) & 0xFF));
    buffer.push_back(static_cast<uint8_t>((value >> 16) & 0xFF));
    buffer.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
    buffer.push_back(static_cast<uint8_t>(value & 0xFF));
}

// 按大端序 从buffer的offset的指定位置读取32为无符号整形，
// 如果剩余字节不足4个，则抛出异常
uint32_t Protocol::ReadUint32BigEndian(const std::vector<uint8_t>& buffer,
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
    // Byte0: version | header_size
    bytes.push_back(BuildByte0(message.header.version, message.header.header_size));
    // Byte1: message_type | flags
    // 为什么最外层还要用static_cast？
    // uint8_t 在参与左移和按位或等表达式时，
    // 会发生整型提升，表达式结果通常会变成 int。
    // 最外层显式转成 uint8_t，是为了把结果明确收窄回一个字节，
    // 和协议头字段的字节级定义保持一致，同时避免依赖隐式转换。
    uint8_t byte1 = static_cast<uint8_t>(
        (Low4(static_cast<uint8_t>(message.header.message_type)) << 4) |
        Low4(static_cast<uint8_t>(message.header.flags)));
    bytes.push_back(byte1);
    // Byte2: serialization | compression
    bytes.push_back(static_cast<uint8_t>(
        (Low4(U8(message.header.serialization)) << 4) |
        Low4(U8(message.header.compression))));

    // bytes3: reserved
    bytes.push_back(message.header.reserved);

    // [payload_size 4B] + [payload]
    AppendUint32BigEndian(bytes, static_cast<uint32_t>(message.payload.size()));
    bytes.insert(bytes.end(), message.payload.begin(), message.payload.end());

    return bytes;
}

// 将字节流解码或结构化消息
// 如果数据非法，抛出异常
ProtocolMessage Protocol::Decode(const std::vector<uint8_t>& data) {
    // 至少需要 4B 头 + 4B payload_size
    if (data.size() < kProtocolHeaderBytes + kPayloadSizeBytes) {
        throw std::runtime_error("protocol data too short");
    }

    ProtocolMessage message;

    //
    uint8_t byte0 = data[0];
    message.header.version = ExtractVersion(byte0);
    message.header.header_size = ExtractHeaderSize(byte0);
    message.header.message_type = static_cast<MessageType>((data[1] >> 4) & 0x0F);
    message.header.flags = static_cast<MessageFlags>(data[1] & 0x0F);
    message.header.serialization = static_cast<SerializationType>((data[2] >> 4) & 0x0F);
    message.header.compression = static_cast<CompressionType>(data[2] & 0x0F);
    message.header.reserved = data[3];

    if (message.header.header_size != 1) {
        throw std::runtime_error("unsupported header_size in minimal protocol");
    }
    // 从data的第kProtocolHeaderBytes个字节位置开始，读取一个4字节的uint32_t
    // 读取payload长度
    uint32_t payload_size = ReadUint32BigEndian(data, kProtocolHeaderBytes);

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

// ---------- 真实业务侧编解码 ----------

// 构造客户端完整请求帧
// [header 4B] [event 4B] [session_id_size 4B + session_id] [payload_size 4B + payload_json]
std::vector<uint8_t> Protocol::BuildFullRequest(
    uint32_t event,
    const std::string& session_id,
    const nlohmann::json& payload) {
    std::vector<uint8_t> bytes;

    AppendPackedHeader(bytes,
                       MessageType::kClientFullRequest,
                       MessageFlags::kMsgWithEvent,
                       SerializationType::kJson,
                       CompressionType::kNone);

    // event 4B
    AppendUint32BigEndian(bytes, event);

    // session_id（连接级事件如 kStartConnection 不带；调用方传空字符串即可）
    if (!session_id.empty()) {
        AppendSizedString(bytes, session_id);
    }

    // [payload_size 4B] + [payload UTF-8]
    const std::string payload_str = payload.dump();
    AppendSizedString(bytes, payload_str);

    return bytes;
}

// 构造客户端音频请求帧
std::vector<uint8_t> Protocol::BuildClientAudioRequest(
    uint32_t event,
    const std::string& session_id,
    const std::vector<uint8_t>& pcm) {
    std::vector<uint8_t> bytes;

    AppendPackedHeader(bytes,
                       MessageType::kClientAudioOnlyRequest,
                       MessageFlags::kMsgWithEvent,
                       SerializationType::kNone,
                       CompressionType::kNone);

    // event 4B（音频帧通常是 events::kTaskRequest=200）
    AppendUint32BigEndian(bytes, event);

    if (!session_id.empty()) {
        AppendSizedString(bytes, session_id);
    }

    // [payload_size 4B] + [PCM 原始字节]
    AppendUint32BigEndian(bytes, static_cast<uint32_t>(pcm.size()));
    AppendBytes(bytes, pcm.data(), pcm.size());

    return bytes;
}

// 解析服务端帧
// optional 段顺序固定：[code(仅错误帧)] [sequence] [event + connect_id|session_id]
// 任何越界 / 不支持的 header_size 都抛 std::runtime_error，由上层 catch 后转 LOG_ERROR
ParsedResponse Protocol::ParseResponse(const std::vector<uint8_t>& data) {
    if (data.size() < kProtocolHeaderBytes) {
        throw std::runtime_error("protocol data too short");
    }

    ParsedResponse parsed;

    // 4 字节 packed 头
    const uint8_t header_size = ExtractHeaderSize(data[0]);
    if (header_size != 1) {
        throw std::runtime_error("unsupported header_size");
    }
    const auto msg_type = static_cast<MessageType>((data[1] >> 4) & 0x0F);
    const auto flags = static_cast<MessageFlags>(data[1] & 0x0F);
    const auto compression = static_cast<CompressionType>(data[2] & 0x0F);
    // serialization / reserved 当前路径用不到，跳过

    std::size_t offset = kProtocolHeaderBytes;

    // [code 4B] —— 仅错误帧
    if (msg_type == MessageType::kServerErrorResponse) {
        parsed.code = static_cast<int32_t>(ReadUint32BigEndian(data, offset));
        offset += kPayloadSizeBytes;
    }

    // [sequence 4B] —— 当前不向上层暴露，仅按位掩码消费掉，避免后续偏移错乱
    if (HasFlag(flags, MessageFlags::kPosSequence) ||
        HasFlag(flags, MessageFlags::kNegSequence)) {
        (void)ReadUint32BigEndian(data, offset);
        offset += kPayloadSizeBytes;
    }

    // [event 4B] +（连接级 → connect_id | 其它 → session_id）
    if (HasFlag(flags, MessageFlags::kMsgWithEvent)) {
        parsed.event = ReadUint32BigEndian(data, offset);
        offset += kPayloadSizeBytes;

        if (IsConnectionLevelEvent(parsed.event)) {
            parsed.connect_id = ReadSizedString(data, offset);
        } else {
            parsed.session_id = ReadSizedString(data, offset);
        }
    }

    // [payload_size 4B] + [payload]
    const uint32_t payload_size = ReadUint32BigEndian(data, offset);
    offset += kPayloadSizeBytes;
    parsed.payload_size = payload_size;
    if (offset + payload_size > data.size()) {
        throw std::runtime_error("protocol payload out of range");
    }

    std::vector<uint8_t> payload_bytes(
        data.begin() + offset,
        data.begin() + offset + payload_size);

    // 服务端可能对大 payload 用 gzip 压缩，需要在交还上层前先解开
    if (compression == CompressionType::kGzip && !payload_bytes.empty()) {
        payload_bytes = DecompressGzip(payload_bytes);
    }

    // ServerAck 用来传二进制（典型场景：TTS PCM 音频）；其它按 JSON 文本处理
    if (msg_type == MessageType::kServerAck) {
        parsed.payload_bytes = std::move(payload_bytes);
        parsed.is_binary = true;
    } else {
        parsed.payload_json.assign(payload_bytes.begin(), payload_bytes.end());
        parsed.is_binary = false;
    }

    return parsed;
}

// ---------- gzip 工具 ----------

// windowBits = 15 + 16：在 raw deflate 流外再套一层 gzip 头/尾
// （+16 这一位是 zlib 文档明确给出的“gzip wrapper”开关）
std::vector<uint8_t> Protocol::CompressGzip(const std::vector<uint8_t>& src) {
    z_stream strm{};
    if (deflateInit2(&strm,
                     Z_DEFAULT_COMPRESSION,
                     Z_DEFLATED,
                     /*windowBits=*/15 + 16,
                     /*memLevel=*/8,
                     Z_DEFAULT_STRATEGY) != Z_OK) {
        throw std::runtime_error("gzip deflateInit2 failed");
    }

    // zlib 接口要求可写指针，但实际只读，const_cast 是惯用写法
    strm.next_in  = const_cast<Bytef*>(src.data());
    strm.avail_in = static_cast<uInt>(src.size());

    std::vector<uint8_t> output;
    constexpr std::size_t kChunk = 4096;
    uint8_t buf[kChunk];

    int ret = Z_OK;
    do {
        strm.next_out  = buf;
        strm.avail_out = kChunk;
        ret = deflate(&strm, Z_FINISH);
        if (ret == Z_STREAM_ERROR) {
            deflateEnd(&strm);
            throw std::runtime_error("gzip deflate stream error");
        }
        const std::size_t produced = kChunk - strm.avail_out;
        output.insert(output.end(), buf, buf + produced);
    } while (strm.avail_out == 0);  // 输出缓冲被填满说明还有剩余，需要继续 deflate

    deflateEnd(&strm);
    if (ret != Z_STREAM_END) {
        throw std::runtime_error("gzip compress incomplete");
    }
    return output;
}

// windowBits = 15 + 32：让 zlib 自动识别 gzip / zlib 两种包头，更鲁棒
std::vector<uint8_t> Protocol::DecompressGzip(const std::vector<uint8_t>& src) {
    if (src.empty()) {
        return {};
    }

    z_stream strm{};
    if (inflateInit2(&strm, /*windowBits=*/15 + 32) != Z_OK) {
        throw std::runtime_error("gzip inflateInit2 failed");
    }

    strm.next_in  = const_cast<Bytef*>(src.data());
    strm.avail_in = static_cast<uInt>(src.size());

    std::vector<uint8_t> output;
    constexpr std::size_t kChunk = 4096;
    uint8_t buf[kChunk];

    int ret = Z_OK;
    do {
        strm.next_out  = buf;
        strm.avail_out = kChunk;
        ret = inflate(&strm, Z_NO_FLUSH);
        if (ret == Z_STREAM_ERROR ||
            ret == Z_NEED_DICT    ||
            ret == Z_DATA_ERROR   ||
            ret == Z_MEM_ERROR) {
            inflateEnd(&strm);
            throw std::runtime_error("gzip inflate failed");
        }
        const std::size_t produced = kChunk - strm.avail_out;
        output.insert(output.end(), buf, buf + produced);
    } while (ret != Z_STREAM_END);

    inflateEnd(&strm);
    return output;
}

}  // namespace interview::common
