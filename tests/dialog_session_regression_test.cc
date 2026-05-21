#include <cassert>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "common/dialog_state.h"
#include "common/protocol.h"
#include "interview/interview_session.h"
#include "services/audio_manager.h"
#include "services/realtime_client.h"

#define private public
#include "interview/dialog_session.h"
#undef private

#include "common/logger.h"
#include "services/mock_llm_client.h"

namespace {

class RecordingRealtimeClient : public interview::services::RealtimeClient {
public:
    bool Connect() override {
        connected_ = true;
        return true;
    }

    void Close() override {
        connected_ = false;
    }

    bool SendEvent(uint32_t event,
                   const std::string& session_id,
                   const nlohmann::json& payload) override {
        last_event = event;
        last_session_id = session_id;
        last_payload = payload;
        return true;
    }

    bool SendAudio(const std::string& session_id,
                   const std::vector<uint8_t>& pcm) override {
        last_audio_session_id = session_id;
        last_audio = pcm;
        return true;
    }

    void SetEventHandler(EventHandler handler) override {
        handler_ = std::move(handler);
    }

    bool IsConnected() const override {
        return connected_;
    }

    uint32_t last_event = 0;
    std::string last_session_id;
    nlohmann::json last_payload;
    std::string last_audio_session_id;
    std::vector<uint8_t> last_audio;

private:
    EventHandler handler_;
    bool connected_ = false;
};

std::unique_ptr<interview::session::InterviewSession> MakeInterviewSession() {
    return std::make_unique<interview::session::InterviewSession>(
        std::make_unique<interview::services::MockLLMClient>());
}

interview::common::ParsedResponse MakeTtsResponse(
    const std::vector<uint8_t>& pcm) {
    interview::common::ParsedResponse evt;
    evt.event = interview::common::events::kTtsResponse;
    evt.session_id = "sid-1";
    evt.is_binary = true;
    evt.payload_bytes = pcm;
    return evt;
}

void TtsResponseQueuesAudioForPlayback() {
    auto realtime = std::make_unique<RecordingRealtimeClient>();
    auto* realtime_ptr = realtime.get();
    interview::session::DialogSession dialog(
        MakeInterviewSession(), std::move(realtime), false);
    (void)realtime_ptr;

    dialog.audio_threads_running_.store(true);
    const std::vector<uint8_t> pcm = {0x01, 0x02, 0x03, 0x04};
    dialog.OnServerEvent(MakeTtsResponse(pcm));

    assert(dialog.tts_queue_.size() == 1);
    assert(dialog.tts_queue_.front() == pcm);
}

void SpeakTextUsesLockedSessionSnapshot() {
    auto realtime = std::make_unique<RecordingRealtimeClient>();
    auto* realtime_ptr = realtime.get();
    interview::session::DialogSession dialog(
        MakeInterviewSession(), std::move(realtime), false);

    {
        std::lock_guard<std::mutex> lock(dialog.data_mutex_);
        dialog.session_id_ = "locked-session-id";
    }

    dialog.SpeakText("hello");

    assert(realtime_ptr->last_event ==
           interview::common::events::kChatTextQuery);
    assert(realtime_ptr->last_session_id == "locked-session-id");
    assert(realtime_ptr->last_payload["content"] == "hello");
}

}  // namespace

int main() {
    assert(interview::common::Logger::Init());
    TtsResponseQueuesAudioForPlayback();
    SpeakTextUsesLockedSessionSnapshot();
    return 0;
}
