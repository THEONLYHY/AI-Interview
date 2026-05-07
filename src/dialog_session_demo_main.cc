
#include "interview/interview_session.h"
#include "services/mock_llm_client.h"
#include "common/logger.h"
#include "interview/dialog_session.h"

#include <iostream>
#include <memory>

int main() {
    if (!Logger::Init()) {
        std::cerr << "logger init failed\n";
        return 1;
    }
    auto llm_client = std::make_unique<MockLLMClient>();
    auto interview_session = std::make_unique<InterviewSession>(std::move(llm_client));
    DialogSession session(std::move(std::move(interview_session)));

    session.Start();
    session.Run();
    session.Stop();
    return 0;
}