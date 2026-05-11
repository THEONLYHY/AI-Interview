#ifndef INCLUDE_SERVICES_MOCK_REALTIME_CLIENT_H_
#define INCLUDE_SERVICES_MOCK_REALTIME_CLIENT_H_

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

#include "services/realtime_client.h"
namespace interview::services {

// MockRealtimeClient：本地脚本化的实时层
// - 构造时注入一组 ParsedResponse 脚本
// - Connect() 后启动工作线程，按 interval 依次回调 handler，模拟服务端事件流
// - SendEvent / SendAudio 仅打日志（不影响脚本投递）
// 用途：跑通 DialogSession 状态机、单元测试、CI（不依赖真豆包）


class MockRealtimeClient : public RealtimeClient {
public:
    explicit MockRealtimeClient(
        std::vector<interview::common::ParsedResponse> script,
        std::chrono::milliseconds interval =  std::chrono::milliseconds(50));
    ~MockRealtimeClient() override;

    bool Connect() override;
    void Close() override;

    bool SendEvent(uint32_t event,
        const std::string& session_id,
        const nlohmann::json& payload) override;

    bool SendAudio(const std::string& session_id,
            const std::vector<uint8_t>& pcm) override;
    
    void SetEventHandler(EventHandler handler) override;
    bool IsConnected() const override;
private:
    void RunDispatchLoop();

    std::vector<interview::common::ParsedResponse> script_;
    std::chrono::milliseconds interval_;
    EventHandler handler_;
    std::thread worker_;
    std::atomic<bool> running_{false};
};

} // namespace interview::services

#endif // INCLUDE_SERVICES_MOCK_REALTIME_CLIENT_H_