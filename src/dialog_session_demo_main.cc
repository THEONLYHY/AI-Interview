
#include "common/config.h"
#include "common/logger.h"
#include "interview/interview_session.h"
#include "interview/dialog_session.h"
#include "services/llm_client.h"
#include "services/mock_llm_client.h"
#include "services/real_llm_client.h"


#include <iostream>
#include <memory>

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

    bool use_real_llm = true;

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

    auto interview_session = std::make_unique<InterviewSession>(std::move(llm_client));
    DialogSession session(std::move(std::move(interview_session)));

    session.Start();
    session.Run();
    session.Stop();
    return 0;
}