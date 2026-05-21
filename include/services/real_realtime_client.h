#ifndef INCLUDE_SERVICES_REAL_REALTIME_CLIENT_H_
#define INCLUDE_SERVICES_REAL_REALTIME_CLIENT_H_

#include <memory>
#include <string>

#include "services/realtime_client.h"

namespace interview::services {

// RealRealtimeClient：豆包 WSS 实现
// Boost.Beast + OpenSSL 真 WSS
// Pimpl 把所有重型第三方头藏在 .cc 里，外面 include 这个头零依赖）

class RealRealtimeClient : public RealtimeClient {
public:
    explicit RealRealtimeClient(std::string server_url);
    ~RealRealtimeClient() override;

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
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace interview::services

#endif  // INCLUDE_SERVICES_REAL_REALTIME_CLIENT_H_