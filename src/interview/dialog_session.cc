#include "interview/dialog_session.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <thread>
#include <utility>
#include <vector>

#include "common/interview_types.h"
#include "common/logger.h"

#include "nlohmann/json.hpp"

namespace interview::session {

using interview::common::DialogState;
using interview::common::DialogStateToString;
using interview::common::EvaluateResult;
using interview::common::InterviewReport;
using interview::common::MessageType;
using interview::common::ParsedResponse;
using interview::common::Question;
namespace events = interview::common::events;
using interview::services::RealtimeClient;

namespace {

// 从 kAsrResult 的 payload_json 中提取真正的识别文本。
//
// 兼容两种来源:
//   - Mock 路径: payload_json 直接就是裸 ASR 文本(无引号、无 {} ),
//     原样返回即可。
//   - Real 路径(豆包 SAMI 实时对话): payload_json 是已解 gzip 的 JSON
//     字符串,真实文本可能在 "text" 或 "results[0].text" 字段。
//
// 任何解析异常都不传播,避免因为字段微调或心跳/空帧把 ASR 链路打断。
// 返回值约定:
//   - 非 JSON 起头(mock 走这里): 原样返回。
//   - JSON 解析失败:           回退,把整段当裸文本返回。
//   - JSON 解析成功但找不到字段:返回空串,上层据此跳过提交。
std::string ExtractAsrText(const std::string& payload_json) {
    if (payload_json.empty()) {
        return {};
    }
    if (payload_json.front() != '{' && payload_json.front() != '[') {
        return payload_json;
    }
    try {
        const auto j = nlohmann::json::parse(payload_json);
        if (j.is_object()) {
            if (auto it = j.find("text");
                it != j.end() && it->is_string()) {
                return it->get<std::string>();
            }
            if (auto it = j.find("results");
                it != j.end() && it->is_array() && !it->empty()) {
                const auto& first = it->front();
                if (first.is_object()) {
                    if (auto t = first.find("text");
                        t != first.end() && t->is_string()) {
                        return t->get<std::string>();
                    }
                }
            }
        }
        return {};
    } catch (const nlohmann::json::exception&) {
        return payload_json;
    }
}

std::vector<uint8_t> Pcm16SamplesToLeBytes(
    const std::vector<int16_t>& samples) {
    std::vector<uint8_t> bytes;
    bytes.reserve(samples.size() * sizeof(int16_t));
    for (const int16_t sample : samples) {
        const auto raw = static_cast<uint16_t>(sample);
        bytes.push_back(static_cast<uint8_t>(raw & 0xFF));
        bytes.push_back(static_cast<uint8_t>((raw >> 8) & 0xFF));
    }
    return bytes;
}

}  // namespace

DialogSession::DialogSession(std::unique_ptr<InterviewSession> interview_session,
                             std::unique_ptr<RealtimeClient> realtime_client,
                             bool audio_enabled)
    : interview_session_(std::move(interview_session)),
      realtime_client_(std::move(realtime_client)),
      audio_enabled_(audio_enabled){}

DialogSession::~DialogSession() {
    Stop();
}

void DialogSession::Start() {
    if (is_running_.load()) {
        return;
    }
    if (interview_session_ == nullptr) {
        LOG_ERROR("interview session is nullptr");
        EmitContent("error", "interview session is nullptr", -1);
        SetState(DialogState::kStopped);
        return;
    }

    is_running_.store(true);
    SetState(DialogState::kConnecting);
    interview_session_->Start();

    LOG_INFO("欢迎参加模拟面试");
    EmitContent("system", "欢迎参加模拟面试", -1);

    if (realtime_client_) {
        // 回调必须先于 Connect，避免 Mock 线程瞬间投递丢失首包。
        realtime_client_->SetEventHandler(
            [this](const ParsedResponse& evt) { OnServerEvent(evt); });

        if (!realtime_client_->Connect()) {
            LOG_ERROR("realtime client connect failed, stopping dialog session");
            EmitContent("error",
                        "realtime client connect failed, stopping dialog session",
                        -1);
            StopAudioThread();
            realtime_client_->Close();
            is_running_.store(false);
            SetState(DialogState::kStopped);
            return;
        }
        LOG_INFO("realtime client connected");

        if (audio_enabled_) {
            SetState(DialogState::kInterviewerSpeaking);
            if (!StartAudioThreads()) {
                LOG_ERROR("voice mode requires working audio input/output; "
                          "realtime session not started");
                EmitContent("error",
                            "voice mode requires working audio input/output; "
                            "realtime session not started",
                            -1);
                realtime_client_->Close();
                is_running_.store(false);
                SetState(DialogState::kStopped);
                return;
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            RequestCurrentQuestionSpeech();
        }
    } else {
        SetState(DialogState::kIdle);
    }
}

void DialogSession::RunFromStdin() {
    if (!is_running_.load()) {
        LOG_WARN("dialog session is not running, call Start() first");
        return;
    }
    while (is_running_.load() && interview_session_->HasNextQuestion()) {
        OnAskQuestion();

        SetState(DialogState::kCandidateSpeaking);
        std::string answer;
        std::getline(std::cin, answer);
        OnCandidateAnswer(answer);
        if (!is_running_.load()) {
            break;
        }

        if (interview_session_->HasPendingFollowup()) {
            SetState(DialogState::kCandidateSpeaking);
            std::string followup_answer;
            std::getline(std::cin, followup_answer);
            OnFollowupAnswer(followup_answer);
            if (!is_running_.load()) {
                break;
            }
        }
        interview_session_->MoveToNextQuestion();
        SetState(DialogState::kIdle);
    }

    if (is_running_.load() && State() != DialogState::kStopped) {
        OnEnterSummary();
    }
}

void DialogSession::Run() {
    RunFromStdin();
}

void DialogSession::RunEventDriven() {
    if (!is_running_.load()) {
        LOG_WARN("dialog session is not running, call Start() first");
        return;
    }
    LOG_INFO("event-driven loop started");
    while (is_running_.load() && State() != DialogState::kCompleted &&
           State() != DialogState::kStopped) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    LOG_INFO("event-driven loop ended, state = {}",
             DialogStateToString(State()));
}

void DialogSession::Stop() {
    const bool was_running = is_running_.exchange(false);

    StopAudioThread();
    if (realtime_client_) {
        realtime_client_->Close();
    }
    SetState(DialogState::kStopped);
    if (was_running) {
        LOG_INFO("dialog session stopped");
    }
}

DialogState DialogSession::State() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return state_;
}

void DialogSession::SetContentCallback(DialogContentCallback callback) {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    content_callback_ = std::move(callback);
}

void DialogSession::SetStateCallback(DialogStateCallback callback) {
    DialogState current_state;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        current_state = state_;
    }

    DialogStateCallback cb;
    {
        std::lock_guard<std::mutex> lock(callback_mutex_);
        state_callback_ = std::move(callback);
        cb = state_callback_;
    }

    // 注册状态回调时立即回放当前状态，UI 初次订阅后可马上刷新状态栏。
    if (cb) {
        cb(current_state);
    }
}

void DialogSession::SetState(DialogState new_state) {
    DialogState old_state;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (state_ == new_state) {
            return;
        }
        old_state = state_;
        state_ = new_state;
    }

    LOG_DEBUG("dialog state changed: {} -> {}",
              DialogStateToString(old_state),
              DialogStateToString(new_state));

    DialogStateCallback cb;
    {
        std::lock_guard<std::mutex> lock(callback_mutex_);
        cb = state_callback_;
    }

    // 回调可能进入 Qt queued connection 或测试断言，不能在 state_mutex_ 内调用。
    if (cb) {
        cb(new_state);
    }
}

void DialogSession::EmitContent(const std::string& role,
                                const std::string& text,
                                int question_index) {
    if (text.empty()) {
        return;
    }

    DialogContentCallback cb;
    {
        std::lock_guard<std::mutex> lock(callback_mutex_);
        cb = content_callback_;
    }

    // 回调可能由后台线程触发；业务层不关心 UI 线程切换。
    if (cb) {
        cb(role, text, question_index);
    }
}

void DialogSession::OnAskQuestion() {
    Question question = interview_session_->GetCurrentQuestion();
    SetState(DialogState::kInterviewerSpeaking);
    LOG_INFO("第{}题：{}", question.id, question.text);
    EmitContent("question", question.text, question.id);
}

void DialogSession::OnCandidateAnswer(const std::string& answer) {
    const Question question = interview_session_->GetCurrentQuestion();
    EmitContent("candidate", answer, question.id);

    SetState(DialogState::kInterviewerThinking);
    EvaluateResult result = interview_session_->SubmitAnswer(answer);

    LOG_INFO("评分：{}", result.score);
    LOG_INFO("反馈：{}", result.feedback);
    EmitContent("feedback",
                "评分：" + std::to_string(result.score) +
                    "\n反馈：" + result.feedback,
                question.id);

    if (result.need_followup) {
        SetState(DialogState::kInterviewerSpeaking);
        LOG_INFO("追问：{}", result.followup_question);
        EmitContent("followup", result.followup_question, question.id);
    }
}

void DialogSession::OnFollowupAnswer(const std::string& answer) {
    const Question question = interview_session_->GetPendingFollowupQuestion();
    const int question_id = question.parent_question_id >= 0
                                ? question.parent_question_id
                                : question.id;
    EmitContent("candidate", answer, question_id);

    SetState(DialogState::kInterviewerThinking);

    EvaluateResult result = interview_session_->SubmitFollowupAnswer(answer);

    LOG_INFO("追问评分：{}", result.score);
    LOG_INFO("追问反馈：{}", result.feedback);
    EmitContent("feedback",
                "追问评分：" + std::to_string(result.score) +
                    "\n追问反馈：" + result.feedback,
                question_id);
}

void DialogSession::OnEnterSummary() {
    SetState(DialogState::kSessionEnding);

    InterviewReport report = interview_session_->GenerateReport();
    SpeakText(report.summary);

    LOG_INFO("面试结束");
    LOG_INFO("总分：{}", report.total_score);
    LOG_INFO("总结：{}", report.summary);
    EmitContent("summary",
                "总分：" + std::to_string(report.total_score) +
                    "\n总结：" + report.summary,
                -1);

    SetState(DialogState::kCompleted);
    is_running_.store(false);
}

void DialogSession::RequestCurrentQuestionSpeech() {
    if (!interview_session_ || !interview_session_->HasNextQuestion()) {
        return;
    }

    Question q = interview_session_->GetCurrentQuestion();
    LOG_INFO("第{}题：{}", q.id, q.text);
    EmitContent("question", q.text, q.id);
    // 这里只是发送文本请求，让服务端开始生成/下发 TTS。
    // 本地播放必须等后续 kTtsResponse(ServerAck/binary) 到达后才发生。
    SpeakText(q.text);
}

void DialogSession::SpeakText(const std::string& text) {
    if (text.empty()) {
        return;
    }
    if (!realtime_client_) {
        LOG_DEBUG("[no realtime] TTS 跳过：{}", text);
        return;
    }
    // 豆包 ChatTextQuery：具体字段以接入时官方文档为准。
    // 这里先拷贝 session_id_ 再发送，避免在网络 write 期间持有 data_mutex_。
    const nlohmann::json payload =
        interview::common::Protocol::BuildReadAloudTextQueryPayload(text);
    std::string session_id;
    {
        std::lock_guard<std::mutex> lock(data_mutex_);
        session_id = session_id_;
    }
    SetState(DialogState::kInterviewerSpeaking);
    tts_cv_.notify_all();
    if (!realtime_client_->SendEvent(events::kChatTextQuery, session_id, payload)) {
        LOG_WARN("SendEvent(kChatTextQuery) failed");
    }
}

void DialogSession::HandleAsrFinalized() {
    if (!is_running_.load()) {
        std::lock_guard<std::mutex> lock(data_mutex_);
        current_asr_text_.clear();
        return;
    }
    SetState(DialogState::kInterviewerThinking);

    std::string asr_text;
    {
        // ASR 文本快照只在锁内复制和清空。后续 LLM 评分可能耗时，
        // 不能在持锁状态下调用 InterviewSession。
        std::lock_guard<std::mutex> lock(data_mutex_);
        asr_text = current_asr_text_;
        current_asr_text_.clear();
    }
    if (asr_text.empty()) {
        LOG_WARN("kAsrEnded but ASR text empty, skip submit");
        return;
    }

    LOG_INFO("候选人回答（ASR）：{}", asr_text);
    const int question_id = interview_session_->HasNextQuestion()
                                ? interview_session_->GetCurrentQuestion().id
                                : -1;
    EmitContent("candidate", asr_text, question_id);

    if (interview_session_->HasPendingFollowup()) {
        EvaluateResult result =
            interview_session_->SubmitFollowupAnswer(asr_text);
        LOG_INFO("追问评分：{}，反馈：{}", result.score, result.feedback);
        EmitContent("feedback",
                    "追问评分：" + std::to_string(result.score) +
                        "\n追问反馈：" + result.feedback,
                    question_id);
        interview_session_->MoveToNextQuestion();
    } else {
        EvaluateResult result = interview_session_->SubmitAnswer(asr_text);
        LOG_INFO("评分：{}，反馈：{}", result.score, result.feedback);
        EmitContent("feedback",
                    "评分：" + std::to_string(result.score) +
                        "\n反馈：" + result.feedback,
                    question_id);

        if (result.need_followup) {
            SetState(DialogState::kInterviewerSpeaking);
            LOG_INFO("追问：{}", result.followup_question);
            EmitContent("followup", result.followup_question, question_id);
            SpeakText(result.followup_question);
            return;
        }
        interview_session_->MoveToNextQuestion();
    }

    if (!interview_session_->HasNextQuestion()) {
        OnEnterSummary();
    } else {
        Question q = interview_session_->GetCurrentQuestion();
        LOG_INFO("第{}题：{}", q.id, q.text);
        EmitContent("question", q.text, q.id);
        SpeakText(q.text);
    }
}

void DialogSession::OnServerEvent(const ParsedResponse& evt) {
    const bool error_response =
        evt.message_type == MessageType::kServerErrorResponse || evt.code != 0;
    if (error_response) {
        LOG_ERROR("server error frame: code={}, json={}", evt.code, evt.payload_json);
        EmitContent("error",
                    "server error frame: code=" + std::to_string(evt.code) +
                        ", payload=" + evt.payload_json,
                    -1);
        SetState(DialogState::kStopped);
        is_running_.store(false);
        StopAudioThread();
        return;
    }
    // kCompleted / kStopped 后会忽略普通后续事件，只允许 session/connection 结束类事件继续处理。
    // 防止迟到的TTS事件会把状态回退。
    const DialogState current_state = State();
    const bool terminal_state = 
                current_state == DialogState::kCompleted ||
                current_state == DialogState::kStopped;
    const bool terminal_event = 
                evt.event == events::kSessionFinished ||
                evt.event == events::kSessionFailed ||
                evt.event == events::kConnectionFailed ||
                evt.event == events::kConnectionFinished;
    if (terminal_state && !terminal_event) {
        LOG_DEBUG("[event] ignored after terminal state: id={}", evt.event);
        return;
    }
    switch (evt.event) {
    case events::kConnectionStarted:
        LOG_INFO("[event] kConnectionStarted connect_id={}", evt.connect_id);
        break;

    case events::kSessionStarted:
        LOG_INFO("[event] kSessionStarted session_id={}", evt.session_id);
        {
            std::lock_guard<std::mutex> lock(data_mutex_);
            session_id_ = evt.session_id;
        }

        SetState(DialogState::kIdle);
        if (!audio_enabled_) {
            RequestCurrentQuestionSpeech();
        }
        break;

    case events::kTtsSentenceStart:
        LOG_INFO("[event] kTtsSentenceStart");
        SetState(DialogState::kInterviewerSpeaking);
        break;

    case events::kTtsResponse:
        if (evt.message_type != MessageType::kServerAck || !evt.is_binary) {
            LOG_WARN("[event] ignored non-binary TTS response: message_type={}, json={}",
                     static_cast<int>(evt.message_type), evt.payload_json);
            break;
        }
        SetState(DialogState::kInterviewerSpeaking);
        LOG_DEBUG("[event] TTS audio chunk bytes={}", evt.payload_bytes.size());
        EnqueueTtsAudio(evt.payload_bytes);
        break;

    case events::kTtsEnded:
        LOG_INFO("[event] kTtsEnded");
        {
            std::lock_guard<std::mutex> lock(tts_mutex_);
            if (!tts_decode_remainder_.empty()) {
                LOG_WARN("discarding incomplete TTS float32 tail bytes={}",
                         tts_decode_remainder_.size());
                tts_decode_remainder_.clear();
            }
        }
        tts_cv_.notify_all();
        MarkTtsPlaybackDrained();
        break;

    case events::kAsrInfo:
        if (State() == DialogState::kInterviewerSpeaking) {
            LOG_DEBUG("[event] ignored ASR start during TTS playback");
            break;
        }
        SetState(DialogState::kCandidateSpeaking);
        {
            std::lock_guard<std::mutex> lock(data_mutex_);
            current_asr_text_.clear();
        }

        break;

    case events::kAsrResult:
        {
            std::string asr_text = ExtractAsrText(evt.payload_json);
            LOG_DEBUG("[event] kAsrResult text='{}', raw={}",
                      asr_text, evt.payload_json);
            std::lock_guard<std::mutex> lock(data_mutex_);
            current_asr_text_ = std::move(asr_text);
        }
        break;

    case events::kAsrEnded:
        if (State() == DialogState::kInterviewerSpeaking) {
            LOG_DEBUG("[event] ignored ASR end during TTS playback");
            break;
        }
        HandleAsrFinalized();
        break;

    case events::kSessionFinished:
        LOG_INFO("[event] kSessionFinished");
        SetState(DialogState::kCompleted);
        is_running_.store(false);
        StopAudioThread();
        break;

    case events::kSessionFailed:
        LOG_ERROR("[event] kSessionFailed payload={}", evt.payload_json);
        EmitContent("error", "session failed: " + evt.payload_json, -1);
        SetState(DialogState::kStopped);
        is_running_.store(false);
        StopAudioThread();
        break;

    case events::kConnectionFailed:
        LOG_ERROR("[event] kConnectionFailed payload={}", evt.payload_json);
        EmitContent("error", "connection failed: " + evt.payload_json, -1);
        SetState(DialogState::kStopped);
        is_running_.store(false);
        StopAudioThread();
        break;

    case events::kConnectionFinished:
        LOG_INFO("[event] kConnectionFinished");
        break;

    case events::kChatResponse:
    case events::kChatQuestionInfo:
    case events::kChatEnded:
    case events::kTtsSentenceEnd:
        LOG_DEBUG("[event] passthrough id={}", evt.event);
        break;

    case events::kUsage:
        // 服务端在每次大模型响应结束后下发 token 计费统计。
        // 当前不做累计，只在 DEBUG 级别落盘，避免 default 分支误报 WARN。
        LOG_DEBUG("[event] kUsage json={}", evt.payload_json);
        break;

    default:
        if (evt.is_binary) {
            LOG_DEBUG("[event] binary id={} bytes={}", evt.event,
                      evt.payload_bytes.size());
        } else {
            LOG_WARN("[event] unhandled id={} json={}", evt.event,
                     evt.payload_json);
        }
        break;
    }
}

bool DialogSession::StartAudioThreads() {
    if (!audio_enabled_) {
        return true;
    }
    if (audio_threads_running_.load()) {
        return true;
    }

    audio_manager_ = std::make_unique<interview::services::AudioManager>();

    try {
        audio_manager_->OpenInputStream();
        audio_manager_->OpenOutputStream();
    } catch (const std::exception& e) {
        LOG_ERROR("audio device startup failed; voice I/O disabled: {}",
                  e.what());
        audio_manager_->Cleanup();
        audio_manager_.reset();
        return false;
    }

    audio_threads_running_.store(true);
    recording_thread_ = std::thread(&DialogSession::RecordingLoop, this);
    playback_thread_ = std::thread(&DialogSession::PlaybackLoop, this);
    LOG_INFO("dialog audio threads started");
    return true;
}

void DialogSession::StopAudioThread() {
    std::lock_guard<std::mutex> lock(stop_mutex_);

    const bool was_running = audio_threads_running_.exchange(false);

    tts_cv_.notify_all();
    if (audio_manager_) {
        audio_manager_->StopInput();
        audio_manager_->StopOutput();
    }

    if (recording_thread_.joinable()) {
        recording_thread_.join();
    }

    if (playback_thread_.joinable()) {
        playback_thread_.join();
    }

    {
        std::lock_guard<std::mutex> lock(tts_mutex_);
        std::queue<std::vector<float>> empty;
        std::swap(tts_queue_, empty);
        tts_decode_remainder_.clear();
    }

    audio_manager_.reset();
    if (was_running) {
        LOG_INFO("dialog audio threads stopped");
    }
}


void DialogSession::RecordingLoop() {
    const bool audio_debug = std::getenv("AI_INTERVIEW_AUDIO_DEBUG") != nullptr;
    auto next_audio_debug_log = std::chrono::steady_clock::now();

    while (audio_threads_running_.load() && is_running_.load()) {
        if (!audio_manager_) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            continue;
        }
        const std::vector<int16_t> samples = audio_manager_->ReadAudio();
        if (samples.empty()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            continue;
        }
        std::vector<uint8_t> pcm = Pcm16SamplesToLeBytes(samples);

        const bool candidate_audio_allowed = CanSendCandidateAudio();
        const auto input_level =
            interview::services::AudioManager::Pcm16LeLevel(pcm);

        if (!candidate_audio_allowed) {
            std::fill(pcm.begin(), pcm.end(), 0);
        }

        const std::string session_id = SessionIdSnapshot();
        if (session_id.empty() || !realtime_client_ ||
            !realtime_client_->IsConnected()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            continue;
        }

        if (!realtime_client_->SendAudio(session_id, pcm)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }

        if (audio_debug) {
            const auto now = std::chrono::steady_clock::now();
            if (now >= next_audio_debug_log) {
                LOG_INFO("[audio-debug] mic mode={} peak={} rms={:.1f} bytes={} state={}",
                         candidate_audio_allowed ? "real" : "silence",
                         input_level.peak,
                         input_level.rms,
                         pcm.size(),
                         DialogStateToString(State()));
                next_audio_debug_log = now + std::chrono::seconds(1);
            }
        }
    }
    LOG_DEBUG("recording loop exited");
}

void DialogSession::PlaybackLoop() {
    while (audio_threads_running_.load()) {
        std::vector<float> samples;
        {
            std::unique_lock<std::mutex> lock(tts_mutex_);
            tts_cv_.wait(lock, [this] {
                return !audio_threads_running_.load() || !tts_queue_.empty();
            });

            if (!audio_threads_running_.load()) {
                break;
            }
            if (!tts_queue_.empty()) {
                samples = std::move(tts_queue_.front());
                tts_queue_.pop();
            }
        }

        if (samples.empty()) {
            continue;
        }
        if (!audio_manager_) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }
        try {
            audio_manager_->WriteAudio(samples);
        } catch (const std::exception& e) {
            LOG_WARN("write output stream failed: {}", e.what());
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
    LOG_DEBUG("playback loop exited");
}

void DialogSession::MarkTtsPlaybackDrained() {
    const DialogState state = State();
    if (state == DialogState::kCompleted || state == DialogState::kStopped) {
        return;
    }
    if (state == DialogState::kInterviewerSpeaking) {
        SetState(DialogState::kIdle);
        LOG_INFO("TTS playback drained; candidate microphone audio enabled");
    }
}

void DialogSession::EnqueueTtsAudio(const std::vector<uint8_t>& pcm) {
    if (!audio_threads_running_.load() || pcm.empty()) {
        return;
    }

    bool queued = false;
    {
        std::lock_guard<std::mutex> lock(tts_mutex_);
        tts_decode_remainder_.insert(tts_decode_remainder_.end(),
                                     pcm.begin(), pcm.end());

        const std::size_t complete_bytes =
            (tts_decode_remainder_.size() / sizeof(float)) * sizeof(float);
        if (complete_bytes > 0) {
            const std::vector<uint8_t> complete(
                tts_decode_remainder_.begin(),
                tts_decode_remainder_.begin() + complete_bytes);
            std::vector<float> samples =
                interview::services::AudioManager::Float32PcmLeToSamples(complete);
            tts_decode_remainder_.erase(
                tts_decode_remainder_.begin(),
                tts_decode_remainder_.begin() + complete_bytes);

            if (!samples.empty()) {
                tts_queue_.push(std::move(samples));
                queued = true;
            }
        }
    }

    if (queued) {
        tts_cv_.notify_one();
    }
}

bool DialogSession::CanSendCandidateAudio() const {
    // 服务端 sami 协议在 session 启动后立刻 VAD 监听 client 音频流。
    // 客户端必须在 kIdle 期间就开始发送 mic PCM,
    // 否则 ~10 秒后服务端抛 DialogAudioIdleTimeoutError。
    //
    // 状态白名单与 doc/ai-interview-staged-roadmap_df117cdc.plan.md
    // 中"录音线程"小节一致:
    //   "会话进入 kIdle 或收到 events::kAsrInfo (说话起首字) 后开始录"
    //
    // kInterviewerSpeaking 期间不发,避免本机扬声器 → mic 的
    // TTS 回音被回送到服务端污染 ASR。
    const DialogState s = State();
    const bool state_allows =
        (s == DialogState::kIdle || s == DialogState::kCandidateSpeaking);
    return is_running_.load() &&
                state_allows &&
                !SessionIdSnapshot().empty() &&
                realtime_client_ != nullptr &&
                realtime_client_->IsConnected();
}

std::string DialogSession::SessionIdSnapshot() const {
    std::lock_guard<std::mutex> lock(data_mutex_);
    return session_id_;
}


}  // namespace interview::session
