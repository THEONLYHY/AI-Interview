#ifndef INCLUDE_SERVICES_REALTIME_CLIENT_H_
#define INCLUDE_SERVICES_REALTIME_CLIENT_H_

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "nlohmann/json.hpp"

#include "common/protocol.h"

namespace interview::services {


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

// RealtimeClient：实时通信客户端抽象基类
// - 具体实现两份：MockRealtimeClient（脚本化测试）、RealRealtimeClient（豆包 WSS）
// - 事件驱动：上层通过 SetEventHandler 注册回调，收到服务端帧后投递 ParsedResponse
class RealtimeClient {
public:
    using EventHandler = 
        std::function<void(const interview::common::ParsedResponse&)>;

        virtual ~RealtimeClient() = default;

        // 建立连接(含WSS 握手 + StartConnection + StartSession)
        // 失败返回false, 不抛异常; 具体错误由实现内部写日志
        virtual bool Connect() = 0;

        // 关闭连接(可重入)，不抛异常
        virtual void Close() = 0;

        // 发送Client Full Request 帧(Json payload)
        //- session_id 为空字符串时不写会话段(适用于kStartConnection 等连接级事件)
        virtual bool SendEvent(uint32_t event,
                                const std::string& session_id,
                                const nlohmann::json& payload) = 0;
        // 发送Client Audio Only帧 （PCM 原始字节)
        // event 通常是 events::kTaskRequest(=200)
        virtual bool SendAudio(const std::string& session_id,
                                const std::vector<uint8_t>& pcm) = 0;
        
        // 注册事件回调
        // 注意：回调在 Connect() 内部启动的工作线程触发，上层需自行处理跨线程访问
        virtual void SetEventHandler(EventHandler handler) = 0;

        virtual bool IsConnected() const = 0;
};

}  // namespace interview::services

#endif
