#include "services/realtime_client.h"

#include <iostream>

namespace interview::services {

using interview::common::Protocol;
using interview::common::ProtocolMessage;

// 构造函数
// 参数：
// server_url ： 实施服务地址
RealTimeClient::RealTimeClient(std::string server_url)
: server_url_(std::move(server_url))
{}

// 析构函数
RealTimeClient::~RealTimeClient() {
    if (connected_) {
        Close();
    }
}

// 建立连接
bool RealTimeClient::Connect() {
    if (connected_) {
        return true;
    }
    if (server_url_.empty()) {
        return false;
    }


    connected_ = true;
    return true;
}

// 关闭连接
void RealTimeClient::Close() {
    if (!connected_) {
        return;
    }
    connected_ = false;
}
// 发送一条协议消息
// 参数:
//   message : 结构化消息
// 返回值：
bool RealTimeClient::SenMessage(const ProtocolMessage& message) {
    if (!connected_) {
        return false;
    }

    std::vector<uint8_t> encoded = Protocol::Encode(message);
    // 当前第四阶段先把编码后的关键信息打印出来，
    // 方便确认 Protocol 层已经被正确接入。
    std::cout << "[RealtimeClient] encoded message bytes = "
                << encoded.size() << "\n";
    std::cout << "[RealtimeClient] header.version = "
                << static_cast<int>(message.header.version) << "\n";
    std::cout << "[RealtimeClient] header.header_size = "
                << static_cast<int>(message.header.header_size) << "\n";
    std::cout << "[RealtimeClient] header.message_type = "
                << static_cast<int>(message.header.message_type) << "\n";
    std::cout << "[RealtimeClient] header.flags = "
                << static_cast<int>(message.header.flags) << "\n";
    std::cout << "[RealtimeClient] header.serialization = "
                << static_cast<int>(message.header.serialization) << "\n";
    std::cout << "[RealtimeClient] payload bytes = "
                << message.payload.size() << "\n";
    return !encoded.empty();
}

bool RealTimeClient::RecvMessage(const std::vector<uint8_t>& raw_data,
                                 ProtocolMessage& message) {
    if (!connected_) {
        return false;
    }
    try {
        message = Protocol::Decode(raw_data);

        if (messgae_handler_) {
            messgae_handler_(message);
        }
        return true;
    } catch (...) {
        return false;
    }
}

void RealTimeClient::SetMessageHandler(MessageHandler handler) {
    messgae_handler_ = std::move(handler);
}

// 获取当前是否已连接
bool RealTimeClient::IsConnected() const {
    return connected_;
}

}  // namespace interview::services
