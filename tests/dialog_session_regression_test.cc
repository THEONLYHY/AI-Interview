#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <queue>
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
        ++connect_calls;
        connected_ = connect_result;
        return connect_result;
    }

    void Close() override {
        ++close_calls;
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
    bool connect_result = true;
    int connect_calls = 0;
    int close_calls = 0;

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
    evt.message_type = interview::common::MessageType::kServerAck;
    evt.event = interview::common::events::kTtsResponse;
    evt.session_id = "sid-1";
    evt.is_binary = true;
    evt.payload_bytes = pcm;
    return evt;
}

interview::common::ParsedResponse MakeEvent(uint32_t event) {
    interview::common::ParsedResponse evt;
    evt.event = event;
    evt.session_id = "sid-1";
    return evt;
}

interview::common::ParsedResponse MakeJsonEvent(
    uint32_t event,
    const std::string& payload_json) {
    interview::common::ParsedResponse evt = MakeEvent(event);
    evt.message_type = interview::common::MessageType::kServerFullResponse;
    evt.payload_json = payload_json;
    return evt;
}

std::vector<uint8_t> Float32LeBytes(std::initializer_list<float> samples) {
    std::vector<uint8_t> bytes;
    bytes.reserve(samples.size() * sizeof(float));
    for (float sample : samples) {
        uint32_t raw = 0;
        static_assert(sizeof(raw) == sizeof(sample));
        std::memcpy(&raw, &sample, sizeof(sample));
        bytes.push_back(static_cast<uint8_t>(raw & 0xFF));
        bytes.push_back(static_cast<uint8_t>((raw >> 8) & 0xFF));
        bytes.push_back(static_cast<uint8_t>((raw >> 16) & 0xFF));
        bytes.push_back(static_cast<uint8_t>((raw >> 24) & 0xFF));
    }
    return bytes;
}

void TtsResponseQueuesFloat32AudioForPlaybackAcrossUnalignedChunks() {
    auto realtime = std::make_unique<RecordingRealtimeClient>();
    auto* realtime_ptr = realtime.get();
    interview::session::DialogSession dialog(
        MakeInterviewSession(), std::move(realtime), false);
    (void)realtime_ptr;

    dialog.audio_threads_running_.store(true);
    const std::vector<uint8_t> pcm = Float32LeBytes({0.25F, -0.5F});
    dialog.OnServerEvent(MakeTtsResponse(
        std::vector<uint8_t>(pcm.begin(), pcm.begin() + 5)));

    assert(dialog.tts_queue_.size() == 1);
    assert(dialog.tts_queue_.front().size() == 1);
    assert(std::fabs(dialog.tts_queue_.front()[0] - 0.25F) < 0.00001F);
    dialog.tts_queue_.pop();
    assert(dialog.tts_decode_remainder_.size() == 1);

    dialog.OnServerEvent(MakeTtsResponse(
        std::vector<uint8_t>(pcm.begin() + 5, pcm.end())));

    assert(dialog.tts_queue_.size() == 1);
    assert(dialog.tts_queue_.front().size() == 1);
    assert(std::fabs(dialog.tts_queue_.front()[0] - -0.5F) < 0.00001F);
    assert(dialog.tts_decode_remainder_.empty());
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
    const std::string content =
        realtime_ptr->last_payload["content"].get<std::string>();
    assert(content.find("直接朗读下面文字") != std::string::npos);
    assert(content.find("hello") != std::string::npos);
    assert(content.find("不要回答") != std::string::npos);
    assert(content != "hello");
    assert(dialog.is_playing_audio_.load());
}

void NonAudioSessionStartedImmediatelySpeaksFirstQuestion() {
    auto realtime = std::make_unique<RecordingRealtimeClient>();
    auto* realtime_ptr = realtime.get();
    interview::session::DialogSession dialog(
        MakeInterviewSession(), std::move(realtime), false);
    dialog.interview_session_->Start();

    dialog.OnServerEvent(MakeEvent(interview::common::events::kSessionStarted));

    assert(realtime_ptr->last_event ==
           interview::common::events::kChatTextQuery);
}

void AudioSessionStartedDefersFirstQuestionUntilAudioIsReady() {
    auto realtime = std::make_unique<RecordingRealtimeClient>();
    auto* realtime_ptr = realtime.get();
    interview::session::DialogSession dialog(
        MakeInterviewSession(), std::move(realtime), true);
    dialog.interview_session_->Start();

    dialog.OnServerEvent(MakeEvent(interview::common::events::kSessionStarted));

    assert(realtime_ptr->last_event !=
           interview::common::events::kChatTextQuery);
    assert(dialog.State() == interview::common::DialogState::kIdle);
}

void TtsEndedMarksPlaybackDrained() {
    auto realtime = std::make_unique<RecordingRealtimeClient>();
    interview::session::DialogSession dialog(
        MakeInterviewSession(), std::move(realtime), true);

    dialog.interview_session_->Start();
    dialog.SetState(interview::common::DialogState::kInterviewerSpeaking);
    dialog.is_playing_audio_.store(true);
    {
        std::lock_guard<std::mutex> lock(dialog.tts_mutex_);
        dialog.tts_decode_remainder_.push_back(0xAA);
    }

    dialog.OnServerEvent(MakeEvent(interview::common::events::kTtsEnded));

    assert(dialog.State() == interview::common::DialogState::kIdle);
    assert(!dialog.is_playing_audio_.load());
    assert(dialog.tts_decode_remainder_.empty());
}

void TtsEndedWaitsForQueuedPlaybackBeforeUnmuting() {
    auto realtime = std::make_unique<RecordingRealtimeClient>();
    interview::session::DialogSession dialog(
        MakeInterviewSession(), std::move(realtime), true);

    dialog.audio_threads_running_.store(true);
    dialog.SetState(interview::common::DialogState::kInterviewerSpeaking);
    dialog.OnServerEvent(MakeTtsResponse(Float32LeBytes({0.25F})));
    dialog.OnServerEvent(MakeEvent(interview::common::events::kTtsEnded));

    assert(dialog.is_playing_audio_.load());
    assert(dialog.State() == interview::common::DialogState::kInterviewerSpeaking);

    {
        std::lock_guard<std::mutex> lock(dialog.tts_mutex_);
        std::queue<std::vector<float>> empty;
        std::swap(dialog.tts_queue_, empty);
    }
    dialog.MarkTtsPlaybackDrained();

    assert(!dialog.is_playing_audio_.load());
    assert(dialog.State() == interview::common::DialogState::kIdle);
}

void AsrFinalizationProcessesAnswerImmediately() {
    auto realtime = std::make_unique<RecordingRealtimeClient>();
    auto* realtime_ptr = realtime.get();
    interview::session::DialogSession dialog(
        MakeInterviewSession(), std::move(realtime), true);
    dialog.interview_session_->Start();
    dialog.is_running_.store(true);
    dialog.audio_threads_running_.store(true);
    {
        std::lock_guard<std::mutex> lock(dialog.data_mutex_);
        dialog.session_id_ = "sid-1";
        dialog.current_asr_text_ =
            "answer from candidate with enough detail to continue";
    }

    dialog.HandleAsrFinalized();

    assert(realtime_ptr->last_event ==
           interview::common::events::kChatTextQuery);
    assert(dialog.current_asr_text_.empty());
}

void ServerChatResponseDoesNotEmitInterviewerContent() {
    auto realtime = std::make_unique<RecordingRealtimeClient>();
    interview::session::DialogSession dialog(
        MakeInterviewSession(), std::move(realtime), true);

    int content_count = 0;
    dialog.SetContentCallback(
        [&](const std::string&, const std::string&, int) {
            ++content_count;
        });

    dialog.OnServerEvent(MakeJsonEvent(
        interview::common::events::kChatResponse,
        R"({"content":"server "})"));
    dialog.OnServerEvent(MakeJsonEvent(
        interview::common::events::kChatResponse,
        R"({"content":"response"})"));

    dialog.OnServerEvent(MakeEvent(interview::common::events::kChatEnded));

    assert(content_count == 0);
}

void TerminalEventsStopAudioThreads() {
    auto realtime = std::make_unique<RecordingRealtimeClient>();
    interview::session::DialogSession dialog(
        MakeInterviewSession(), std::move(realtime), true);

    dialog.audio_threads_running_.store(true);
    dialog.OnServerEvent(MakeEvent(interview::common::events::kSessionFinished));

    assert(!dialog.audio_threads_running_.load());
    assert(dialog.State() == interview::common::DialogState::kCompleted);
}

void StartStopsWhenRealtimeConnectFails() {
    auto realtime = std::make_unique<RecordingRealtimeClient>();
    auto* realtime_ptr = realtime.get();
    realtime_ptr->connect_result = false;
    interview::session::DialogSession dialog(
        MakeInterviewSession(), std::move(realtime), false);

    dialog.Start();

    assert(realtime_ptr->connect_calls == 1);
    assert(!dialog.is_running_.load());
    assert(dialog.State() == interview::common::DialogState::kStopped);
}

void StartDoesNotConnectAfterInterviewSessionMissing() {
    auto realtime = std::make_unique<RecordingRealtimeClient>();
    auto* realtime_ptr = realtime.get();
    interview::session::DialogSession dialog(
        nullptr, std::move(realtime), false);

    dialog.Start();

    assert(realtime_ptr->connect_calls == 0);
    assert(!dialog.is_running_.load());
    assert(dialog.State() == interview::common::DialogState::kStopped);
}

}  // namespace

int main() {
    assert(interview::common::Logger::Init());
    TtsResponseQueuesFloat32AudioForPlaybackAcrossUnalignedChunks();
    SpeakTextUsesLockedSessionSnapshot();
    NonAudioSessionStartedImmediatelySpeaksFirstQuestion();
    AudioSessionStartedDefersFirstQuestionUntilAudioIsReady();
    TtsEndedMarksPlaybackDrained();
    TtsEndedWaitsForQueuedPlaybackBeforeUnmuting();
    AsrFinalizationProcessesAnswerImmediately();
    ServerChatResponseDoesNotEmitInterviewerContent();
    TerminalEventsStopAudioThreads();
    StartStopsWhenRealtimeConnectFails();
    StartDoesNotConnectAfterInterviewSessionMissing();
    return 0;
}
