#include "services/mock_realtime_client.h"

#include <exception>
#include <thread>
#include <utility>

#include "common/logger.h"

namespace interview::services {

MockRealtimeClient::MockRealtimeClient(
    std::vector<interview::common::ParsedResponse> script,
    std::chrono::milliseconds interval)
: script_(std::move(script))
, interval_(interval) {

}

bool MockRealtimeClient::Connect() {
    // 用 atomic exchange 保证 Connect 重入只启动一个线程
    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true)) {
        return true;
    }

    worker_ = std::thread(&MockRealtimeClient::RunDispatchLoop, this);
    LOG_INFO("[MockRealtimeClient] connected, script size = {}", script_.size());
    return true;
}

void MockRealtimeClient::Close() {
    // 设标志 + join；exchange 保证多次 Close 只 join 一次
    if (!running_.exchange(false)) {
        return;
    }
    if (worker_.joinable()) {
        worker_.join();
    }
    LOG_INFO("[MockRealtimeClient] closed");
}

MockRealtimeClient::~MockRealtimeClient() {
    Close();
}

bool MockRealtimeClient::SendEvent(uint32_t event,
    const std::string& session_id,
    const nlohmann::json& payload) {
    if (!running_.load()) {
        LOG_WARN("[MockRealtimeClient] SendEvent on closed client, event = {}", event);
        return false;
    }
    LOG_DEBUG("[MockRealtimeClient] SendEvent event = {}, session_id = '{}', payload = {}",
        event, session_id, payload.dump());
    return true;
}

bool MockRealtimeClient::SendAudio(const std::string& session_id,
        const std::vector<uint8_t>& pcm) {
    if (!running_.load()) {
        LOG_WARN("[MockRealtimeClient] SendAudio on closed client");
        return false;
    }
    LOG_DEBUG("[MockRealtimeClient] SendAudio session_id = '{}', pcm bytes = {}",
                session_id, pcm.size());
    return true;        
}

void MockRealtimeClient::SetEventHandler(EventHandler handler) {
    // 约定：在 Connect() 前设置，运行期不再改，所以无需加锁
    handler_ = std::move(handler);
}

bool MockRealtimeClient::IsConnected() const {
    return running_.load();
} 

// 按脚本顺序投递事件；每条事件之间 sleep interval_
// 中途 Close() 触发 running_ = false，立刻停止
void MockRealtimeClient::RunDispatchLoop() {
    for (const auto& event :script_) {
        if (!running_.load()) {
            break;
        }
        std::this_thread::sleep_for(interval_);
        if (!running_.load()) {
            break;
        }
        if (handler_) {
            // 用户回调里抛异常不能把工作线程带飞，这里 catch + 记日志
            try {
                handler_(event);
            } catch (const std::exception& e) {
                LOG_ERROR("[MockRealtimeClient] event handler threw: {}", e.what());
            } catch (...) {
                LOG_ERROR("[MockRealtimeClient] event handler threw unknown exception");
            }
        }
    }
    LOG_DEBUG("[MockRealtimeClient] dispatch loop exited");
}

}  // namespace interview::services

