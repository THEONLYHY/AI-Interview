#include <chrono>
#include <memory>
#include <utility>
#include <vector>

#include "common/logger.h"
#include "common/protocol.h"
#include "interview/dialog_session.h"
#include "interview/interview_session.h"
#include "services/mock_llm_client.h"
#include "services/mock_realtime_client.h"

using namespace interview;

// 阶段 5：MockRealtimeClient 脚本跑通事件驱动状态机（不依赖真网络）

namespace {

common::ParsedResponse MakeEvent(uint32_t event_id,
                                 std::string session_id = {},
                                 std::string payload_json = {}) {
    common::ParsedResponse e;
    e.event = event_id;
    e.session_id = std::move(session_id);
    e.payload_json = std::move(payload_json);
    return e;
}

}  // namespace

int main() {
    if (!common::Logger::Init()) {
        return 1;
    }

    auto llm_client = std::make_unique<services::MockLLMClient>();
    auto interview_session =
        std::make_unique<session::InterviewSession>(std::move(llm_client));

    const std::string kSid = "mock-session-1";
    std::vector<common::ParsedResponse> script;
    script.push_back(MakeEvent(common::events::kConnectionStarted));
    script.back().connect_id = "mock-connect-1";
    script.push_back(MakeEvent(common::events::kSessionStarted, kSid));

    // InterviewSession::Start() 默认拉 3 道主问题；MockLLM 通常不产生追问，
    // 故每题一轮 ASR（Info → Result → End）即可推进到下一题。
    const char* asr_texts[] = {
        "我熟悉 C++ 的智能指针和 RAII。",
        "我了解 STL 容器与时间复杂度。",
        "我用过多线程与互斥锁做并发控制。",
    };
    for (const char* text : asr_texts) {
        script.push_back(MakeEvent(common::events::kAsrInfo, kSid));
        script.push_back(MakeEvent(common::events::kAsrResult, kSid, text));
        script.push_back(MakeEvent(common::events::kAsrEnded, kSid));
    }
    script.push_back(MakeEvent(common::events::kSessionFinished, kSid));

    auto realtime_client = std::make_unique<services::MockRealtimeClient>(
        std::move(script), std::chrono::milliseconds(200));

    session::DialogSession dialog_session(std::move(interview_session),
                                          std::move(realtime_client));

    dialog_session.Start();
    dialog_session.RunEventDriven();
    dialog_session.Stop();

    return 0;
}
