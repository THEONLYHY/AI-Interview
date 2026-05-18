#include <chrono>
#include <iostream>
#include <memory>
#include <vector>

#include "common/config.h"
#include "common/logger.h"
#include "common/protocol.h"
#include "interview/dialog_session.h"
#include "interview/interview_session.h"
#include "services/llm_client.h"
#include "services/mock_llm_client.h"
#include "services/mock_realtime_client.h"
#include "services/real_llm_client.h"

using namespace interview::common;
using namespace interview::services;
using namespace interview::session;

namespace {

ParsedResponse MakeEvent(uint32_t event_id,
                         std::string session_id = {},
                         std::string payload_json = {}) {
    ParsedResponse e;
    e.event = event_id;
    e.session_id = std::move(session_id);
    e.payload_json = std::move(payload_json);
    return e;
}

}  // namespace

int main() {
    if (!Logger::Init()) {
        std::cerr << "logger init failed\n";
        return 1;
    }

    AppConfig config;
    try {
        config = LoadConfig("config/default_config.json");
    } catch (const std::exception& e) {
        LOG_ERROR("load config failed : {}", e.what());
        return 1;
    }

    const bool use_real_llm = true;
    std::unique_ptr<LLMClient> llm_client;

    if (use_real_llm) {
        LOG_INFO("using RealLLMClient");
        llm_client = std::make_unique<RealLLMClient>(
            config.llm.api_url,
            config.llm.api_key,
            config.llm.model,
            config.llm.temperature,
            config.llm.max_tokens,
            config.llm.timeout_seconds);
    } else {
        LOG_INFO("using MockLLMClient");
        llm_client = std::make_unique<MockLLMClient>();
    }

    auto interview_session =
        std::make_unique<InterviewSession>(std::move(llm_client));

    // 阶段 5：Mock 实时层 + 仅握手两帧；stdin 仍走完整文本问答
    constexpr const char kMockSession[] = "mock-session-1";
    std::vector<ParsedResponse> script;
    script.push_back(MakeEvent(events::kConnectionStarted));
    script.back().connect_id = "mock-connect-1";
    script.push_back(MakeEvent(events::kSessionStarted, kMockSession));

    auto realtime_client = std::make_unique<MockRealtimeClient>(
        std::move(script), std::chrono::milliseconds(80));

    DialogSession session(std::move(interview_session),
                          std::move(realtime_client));

    session.Start();
    session.Run();
    session.Stop();
    return 0;
}
