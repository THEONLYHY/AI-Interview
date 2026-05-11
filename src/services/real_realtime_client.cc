#include "services/real_realtime_client.h"
#include "common/logger.h"

#include <utility>


namespace interview::services {
    
// Impl：阶段 5 占位实现
// 阶段 7 这里会接入 boost::asio::io_context / ssl::context / websocket::stream
//        + recv_thread_ + std::atomic<bool> running_ + std::mutex send_mutex_
class RealRealtimeClient::Impl {
public:
    using EventHandler = RealtimeClient::EventHandler;

    explicit Impl(std::string server_url)
        : server_url_(std::move(server_url)) {}

    bool Connect() {
        // 阶段 5：直接返回 false，避免上层误以为已经连上豆包
        LOG_WARN("[RealRealtimeClient] Connect() not implemented yet (stage 7), url = {}",
            server_url_);
        return false;
    }

    void Close() {

    }

    bool SendEvent(uint32_t event,
        const std::string& /*session_id*/,
        const nlohmann::json& /*payload*/) {
        LOG_WARN("[RealRealtimeClient] SendEvent(event = {}) not implemented yet (stage 7)",
                event);
        return false;
    }

    bool SendAudio(const std::string& /*session_id*/,
            const std::vector<uint8_t>& pcm) {
        LOG_WARN("[RealRealtimeClient] SendAudio(pcm bytes = {}) not implemented yet (stage 7)",
                pcm.size());
        return false;
    }

    void SetEventHandler(EventHandler handler) {
        handler_ = std::move(handler);
    }

    bool IsConnected() const {
        return false;
    }    
private:
    std::string server_url_;
    EventHandler handler_;
};


// ===== 外壳方法（仅做 impl_ 转发） =====

RealRealtimeClient::RealRealtimeClient(std::string server_url)
    : impl_(std::make_unique<Impl>(std::move(server_url))) {}

// 必须在 .cc 里定义析构：unique_ptr<Impl> 需要 Impl 完整类型
// 写在头里会因为 Impl 是 forward declare 而编译失败
RealRealtimeClient::~RealRealtimeClient() = default;

bool RealRealtimeClient::Connect() {
    return impl_->Connect();
}
void RealRealtimeClient::Close() {
    impl_->Close();
}

bool RealRealtimeClient::SendEvent(uint32_t event,
    const std::string& session_id,
    const nlohmann::json& payload) {
    return impl_->SendEvent(event, session_id, payload);
}

bool RealRealtimeClient::SendAudio(const std::string& session_id,
    const std::vector<uint8_t>& pcm) {
    return impl_->SendAudio(session_id, pcm);
}

void RealRealtimeClient::SetEventHandler(EventHandler handler) {
    impl_->SetEventHandler(std::move(handler));
}

bool RealRealtimeClient::IsConnected() const {
    return impl_->IsConnected();
}


}