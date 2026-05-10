#ifndef INCLUDE_SERVICES_REALTIME_CLIENT_H_
#define INCLUDE_SERVICES_REALTIME_CLIENT_H_

#include <string>
#include <vector>
#include <functional>

#include "common/protocol.h"

namespace interview::services {

// RealtimeClient 表示实时通信客户端。
// 第四阶段当前先做最小骨架，它的职责先只包括：
// 1. 建立连接
// 2. 关闭连接
// 3. 发送协议消息
//
// 后面再逐步扩展：
// - 接收服务端消息
// - 回调上层处理
// - WebSocket 连接管理
// - 音频流收发

class RealTimeClient {
public:
    // 注册一个回调函数，在收到并成功解析消息后触发
    using MessageHandler = std::function<void(const interview::common::ProtocolMessage&)>;
    // 构造函数
    // 参数：
    // server_url ： 实施服务地址
    explicit RealTimeClient(std::string server_url);

    // 析构函数
    ~RealTimeClient();

    // 建立连接
    bool Connect();

    // 关闭连接
    void Close();
    // 发送一条协议消息
    // 参数:
    //   message : 结构化消息
    // 返回值：
    bool SenMessage(const interview::common::ProtocolMessage& message);
    bool RecvMessage(const std::vector<uint8_t>& raw_data,
                     interview::common::ProtocolMessage& message);
    // 设置消息处理回调
    void SetMessageHandler(MessageHandler handler);
    // 获取当前是否已连接
    bool IsConnected() const;

private:
    // 服务端地址
    std::string server_url_;
    // 当前连接状态
    bool connected_ = false;
    MessageHandler messgae_handler_;
};

}  // namespace interview::services

#endif
