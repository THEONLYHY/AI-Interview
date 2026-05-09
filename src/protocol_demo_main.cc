#include <iostream>
#include <string>
#include <vector>

#include "common/protocol.h"

// 把字符串转成字节数组。
std::vector<uint8_t> StringToBytes(const std::string& text) {
  return std::vector<uint8_t>(text.begin(), text.end());
}

// 把字节数组转回字符串。
std::string BytesToString(const std::vector<uint8_t>& bytes) {
  return std::string(bytes.begin(), bytes.end());
}

// 打印一条分隔线，方便观察输出。
void PrintSeparator(const std::string& title) {
  std::cout << "\n========== " << title << " ==========\n";
}

// 测试正常路径：
// 构造一条消息，先编码，再解码，验证结果是否一致。
void TestNormalEncodeDecode() {
  PrintSeparator("normal encode/decode");

  ProtocolMessage message;
  message.header.version = kProtocolVersion;
  message.header.header_size = 1;
  message.header.message_type = MessageType::kClientEvent;
  message.header.flags = 0;
  message.header.serialization = SerializationType::kJson;
  message.payload = StringToBytes("hello");

  std::vector<uint8_t> encoded = Protocol::Encode(message);

  std::cout << "encoded bytes size = " << encoded.size() << "\n";
  std::cout << "encoded bytes: ";
  for (uint8_t byte : encoded) {
    std::cout << static_cast<int>(byte) << " ";
  }
  std::cout << "\n";

  ProtocolMessage decoded = Protocol::Decode(encoded);

  std::cout << "decoded header.version = "
            << static_cast<int>(decoded.header.version) << "\n";
  std::cout << "decoded header.header_size = "
            << static_cast<int>(decoded.header.header_size) << "\n";
  std::cout << "decoded header.message_type = "
            << static_cast<int>(decoded.header.message_type) << "\n";
  std::cout << "decoded header.flags = "
            << static_cast<int>(decoded.header.flags) << "\n";
  std::cout << "decoded header.serialization = "
            << static_cast<int>(decoded.header.serialization) << "\n";
  std::cout << "decoded payload = "
            << BytesToString(decoded.payload) << "\n";
}

// 测试异常路径：数据太短。
// 当前最小协议至少需要：4 字节固定头 + 4 字节 payload 长度字段。
void TestTooShortData() {
  PrintSeparator("too short data");

  std::vector<uint8_t> bad_data = {17, 1, 0};

  try {
    ProtocolMessage decoded = Protocol::Decode(bad_data);
    (void)decoded;
    std::cout << "unexpected success\n";
  } catch (const std::exception& e) {
    std::cout << "caught exception: " << e.what() << "\n";
  }
}

// 测试异常路径：payload 长度不匹配。
// 这里故意写 payload_size = 10，但后面实际只给 3 个字节。
void TestPayloadSizeMismatch() {
  PrintSeparator("payload size mismatch");

  std::vector<uint8_t> bad_data;

  // 固定头。
  bad_data.push_back(Protocol::BuildByte0(kProtocolVersion, 1));
  bad_data.push_back(static_cast<uint8_t>(MessageType::kClientEvent));
  bad_data.push_back(0);
  bad_data.push_back(static_cast<uint8_t>(SerializationType::kJson));

  // payload size = 10
  Protocol::AppendUint32(bad_data, 10);

  // 实际 payload 只给 3 个字节。
  bad_data.push_back('a');
  bad_data.push_back('b');
  bad_data.push_back('c');

  try {
    ProtocolMessage decoded = Protocol::Decode(bad_data);
    (void)decoded;
    std::cout << "unexpected success\n";
  } catch (const std::exception& e) {
    std::cout << "caught exception: " << e.what() << "\n";
  }
}

// 测试异常路径：非法 header_size。
// 当前最小实现只支持 header_size = 1。
void TestUnsupportedHeaderSize() {
  PrintSeparator("unsupported header_size");

  std::vector<uint8_t> bad_data;

  // 故意构造 header_size = 2。
  bad_data.push_back(Protocol::BuildByte0(kProtocolVersion, 2));
  bad_data.push_back(static_cast<uint8_t>(MessageType::kClientEvent));
  bad_data.push_back(0);
  bad_data.push_back(static_cast<uint8_t>(SerializationType::kJson));

  // payload size = 5
  Protocol::AppendUint32(bad_data, 5);

  // payload = "hello"
  std::vector<uint8_t> payload = StringToBytes("hello");
  bad_data.insert(bad_data.end(), payload.begin(), payload.end());

  try {
    ProtocolMessage decoded = Protocol::Decode(bad_data);
    (void)decoded;
    std::cout << "unexpected success\n";
  } catch (const std::exception& e) {
    std::cout << "caught exception: " << e.what() << "\n";
  }
}

int main() {
  TestNormalEncodeDecode();
  TestTooShortData();
  TestPayloadSizeMismatch();
  TestUnsupportedHeaderSize();

  return 0;
}