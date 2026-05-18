#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "common/protocol.h"

namespace {

using interview::common::CompressionType;
using interview::common::MessageFlags;
using interview::common::MessageType;
using interview::common::ParsedResponse;
using interview::common::Protocol;
using interview::common::ProtocolMessage;
using interview::common::SerializationType;
namespace events = interview::common::events;

void AppendSizedStringUtf8(std::vector<uint8_t>* buffer, const std::string& s) {
    Protocol::AppendUint32BigEndian(*buffer, static_cast<uint32_t>(s.size()));
    buffer->insert(buffer->end(), s.begin(), s.end());
}

std::vector<uint8_t> MakeMinimalServerFullResponseWithSessionEvent(
        uint32_t event_id,
        const std::string& session_id,
        const std::string& payload_json_utf8) {
    std::vector<uint8_t> data;
    data.reserve(64);
    data.push_back(Protocol::BuildByte0(1, 1));
    const uint8_t msg = static_cast<uint8_t>(MessageType::kServerFullResponse) & 0x0F;
    const uint8_t flg = static_cast<uint8_t>(MessageFlags::kMsgWithEvent) & 0x0F;
    data.push_back(static_cast<uint8_t>((msg << 4) | flg));
    const uint8_t ser = static_cast<uint8_t>(SerializationType::kJson) & 0x0F;
    const uint8_t comp = static_cast<uint8_t>(CompressionType::kNone) & 0x0F;
    data.push_back(static_cast<uint8_t>((ser << 4) | comp));
    data.push_back(0);

    Protocol::AppendUint32BigEndian(data, event_id);
    AppendSizedStringUtf8(&data, session_id);
    Protocol::AppendUint32BigEndian(
            data, static_cast<uint32_t>(payload_json_utf8.size()));
    data.insert(data.end(), payload_json_utf8.begin(), payload_json_utf8.end());
    return data;
}

TEST(ProtocolSmoke, BuildByte0RoundTrip) {
    const uint8_t b = Protocol::BuildByte0(1, 1);
    EXPECT_EQ(Protocol::ExtractVersion(b), 1);
    EXPECT_EQ(Protocol::ExtractHeaderSize(b), 1);
}

TEST(ProtocolSmoke, EncodeDecodeRoundTrip) {
    ProtocolMessage message;
    message.header.version = interview::common::kProtocolVersion;
    message.header.header_size = 1;
    message.header.message_type = MessageType::kClientFullRequest;
    message.header.flags = MessageFlags::kNoSequence;
    message.header.serialization = SerializationType::kJson;
    message.header.compression = CompressionType::kNone;
    message.header.reserved = 0;
    message.payload = {'h', 'e', 'l', 'l', 'o'};

    const std::vector<uint8_t> encoded = Protocol::Encode(message);
    const ProtocolMessage decoded = Protocol::Decode(encoded);

    EXPECT_EQ(decoded.header.version, message.header.version);
    EXPECT_EQ(decoded.header.header_size, message.header.header_size);
    EXPECT_EQ(decoded.header.message_type, message.header.message_type);
    EXPECT_EQ(static_cast<int>(decoded.header.flags),
              static_cast<int>(message.header.flags));
    EXPECT_EQ(decoded.header.serialization, message.header.serialization);
    EXPECT_EQ(decoded.header.compression, message.header.compression);
    EXPECT_EQ(decoded.payload, message.payload);
}

TEST(ProtocolSmoke, GzipRoundTrip) {
    const std::vector<uint8_t> plain = {0x00, 0x01, 0xFE, 0xFF};
    const std::vector<uint8_t> compressed = Protocol::CompressGzip(plain);
    ASSERT_FALSE(compressed.empty());
    const std::vector<uint8_t> restored = Protocol::DecompressGzip(compressed);
    EXPECT_EQ(restored, plain);
}

TEST(ProtocolSmoke, DecodeTooShortThrows) {
    const std::vector<uint8_t> bad = {17, 1, 0, 0};
    EXPECT_THROW((void)Protocol::Decode(bad), std::runtime_error);
}

TEST(ProtocolSmoke, DecodePayloadSizeMismatchThrows) {
    std::vector<uint8_t> bad;
    bad.push_back(Protocol::BuildByte0(1, 1));
    bad.push_back(static_cast<uint8_t>(
            (static_cast<uint8_t>(MessageType::kClientFullRequest) << 4) |
            static_cast<uint8_t>(MessageFlags::kNoSequence)));
    bad.push_back(static_cast<uint8_t>(
            (static_cast<uint8_t>(SerializationType::kJson) << 4) |
            static_cast<uint8_t>(CompressionType::kNone)));
    bad.push_back(0);
    Protocol::AppendUint32BigEndian(bad, 10);
    bad.push_back('a');
    bad.push_back('b');
    bad.push_back('c');
    EXPECT_THROW((void)Protocol::Decode(bad), std::runtime_error);
}

TEST(ProtocolSmoke, DecodeUnsupportedHeaderSizeThrows) {
    std::vector<uint8_t> bad;
    bad.push_back(Protocol::BuildByte0(1, 2));
    bad.push_back(static_cast<uint8_t>(
            (static_cast<uint8_t>(MessageType::kClientFullRequest) << 4) |
            static_cast<uint8_t>(MessageFlags::kNoSequence)));
    bad.push_back(static_cast<uint8_t>(
            (static_cast<uint8_t>(SerializationType::kJson) << 4) |
            static_cast<uint8_t>(CompressionType::kNone)));
    bad.push_back(0);
    const std::string payload = "hello";
    Protocol::AppendUint32BigEndian(bad, static_cast<uint32_t>(payload.size()));
    bad.insert(bad.end(), payload.begin(), payload.end());
    EXPECT_THROW((void)Protocol::Decode(bad), std::runtime_error);
}

TEST(ProtocolSmoke, Uint32BigEndianRoundTrip) {
    std::vector<uint8_t> buf;
    Protocol::AppendUint32BigEndian(buf, 0x01020304);
    ASSERT_GE(buf.size(), 4u);
    EXPECT_EQ(Protocol::ReadUint32BigEndian(buf, 0), 0x01020304u);
}

TEST(ProtocolSmoke, ReadUint32BigEndianTooSmallThrows) {
    std::vector<uint8_t> buf = {1, 2, 3};
    EXPECT_THROW((void)Protocol::ReadUint32BigEndian(buf, 0), std::out_of_range);
}

TEST(ProtocolSmoke, BuildFullRequestStartsWithVersionOne) {
    const nlohmann::json payload = {{"k", "v"}};
    const std::vector<uint8_t> bytes =
            Protocol::BuildFullRequest(events::kStartSession, "sid-1", payload);
    ASSERT_GE(bytes.size(), 4u);
    EXPECT_EQ(Protocol::ExtractVersion(bytes[0]), 1);
    EXPECT_EQ(Protocol::ExtractHeaderSize(bytes[0]), 1);
}

// ParseResponse 在非连接级事件下总会读取 session_id 段；须与 BuildFullRequest 非空 session_id 对齐。
TEST(ProtocolSmoke, BuildFullRequestParseResponseRoundTrip) {
    const nlohmann::json payload = {{"role", "unit"}, {"n", 3}};
    const std::vector<uint8_t> bytes =
            Protocol::BuildFullRequest(events::kStartSession, "sid-z", payload);
    const ParsedResponse parsed = Protocol::ParseResponse(bytes);
    EXPECT_EQ(parsed.event, events::kStartSession);
    EXPECT_EQ(parsed.session_id, "sid-z");
    EXPECT_EQ(parsed.payload_json, payload.dump());
}

TEST(ProtocolSmoke, BuildClientAudioRequestUsesAudioOnlyMessageType) {
    const std::vector<uint8_t> pcm = {9, 8, 7};
    const std::vector<uint8_t> bytes = Protocol::BuildClientAudioRequest(
            events::kTaskRequest, "sess-a", pcm);
    ASSERT_GE(bytes.size(), 4u);
    const auto mt =
            static_cast<MessageType>((bytes[1] >> 4) & 0x0F);
    EXPECT_EQ(mt, MessageType::kClientAudioOnlyRequest);
}

TEST(ProtocolSmoke, ParseResponseSessionStartedWithJson) {
    const std::string sid = "mock-session-xyz";
    const std::string json = R"({"ok":true})";
    const std::vector<uint8_t> frame =
            MakeMinimalServerFullResponseWithSessionEvent(
                    events::kSessionStarted, sid, json);
    const ParsedResponse parsed = Protocol::ParseResponse(frame);
    EXPECT_EQ(parsed.event, events::kSessionStarted);
    EXPECT_EQ(parsed.session_id, sid);
    EXPECT_EQ(parsed.payload_json, json);
    EXPECT_FALSE(parsed.is_binary);
}

TEST(ProtocolSmoke, ParseResponseServerAckBinaryPayload) {
    const std::vector<uint8_t> pcm = {0x00, 0xFF, 0x80};
    std::vector<uint8_t> frame;
    frame.push_back(Protocol::BuildByte0(1, 1));
    const uint8_t b1 = static_cast<uint8_t>(
            ((static_cast<uint8_t>(MessageType::kServerAck) & 0x0F) << 4) |
            (static_cast<uint8_t>(MessageFlags::kMsgWithEvent) & 0x0F));
    frame.push_back(b1);
    frame.push_back(static_cast<uint8_t>(
            ((static_cast<uint8_t>(SerializationType::kNone) & 0x0F) << 4) |
            (static_cast<uint8_t>(CompressionType::kNone) & 0x0F)));
    frame.push_back(0);
    Protocol::AppendUint32BigEndian(frame, events::kTtsResponse);
    AppendSizedStringUtf8(&frame, "tts-sid");
    Protocol::AppendUint32BigEndian(frame, static_cast<uint32_t>(pcm.size()));
    frame.insert(frame.end(), pcm.begin(), pcm.end());

    const ParsedResponse parsed = Protocol::ParseResponse(frame);
    EXPECT_EQ(parsed.event, events::kTtsResponse);
    EXPECT_EQ(parsed.session_id, "tts-sid");
    EXPECT_TRUE(parsed.is_binary);
    EXPECT_EQ(parsed.payload_bytes, pcm);
}

}  // namespace
