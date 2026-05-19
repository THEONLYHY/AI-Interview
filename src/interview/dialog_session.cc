#include "interview/dialog_session.h"

#include <chrono>
#include <iostream>
#include <thread>
#include <utility>

#include "common/interview_types.h"
#include "common/logger.h"

#include "nlohmann/json.hpp"

namespace interview::session {

using interview::common::DialogState;
using interview::common::DialogStateToString;
using interview::common::EvaluateResult;
using interview::common::InterviewReport;
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
    is_running_.store(true);
    SetState(DialogState::kConnecting);

    if (interview_session_) {
        interview_session_->Start();
    }
    
    OnInterviewStarted();

    if (realtime_client_) {
        // 回调必须先于 Connect，避免 Mock 线程瞬间投递丢失首包
        realtime_client_->SetEventHandler(
            [this](const ParsedResponse& evt) { OnServerEvent(evt); });
            
        // 音频优先于Connect 启动。 RealRealtimeClient::Connect() 内部会发送
        // StartSession, 服务端可能很快返回首题 TTS；提前启动播放队列可避免首包丢失。
        StartAudioThreads();

        const bool connected = realtime_client_->Connect();
        if (!connected) {
            LOG_WARN("realtime client connect failed, continue text-only path");
            StopAudioThread();
            SetState(DialogState::kIdle);
        } else {
            LOG_INFO("realtime client connected");
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
    while (is_running_.load() && state_ != DialogState::kCompleted &&
           State() != DialogState::kStopped) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    LOG_INFO("event-driven loop ended, state = {}",
             DialogStateToString(state_));
}

void DialogSession::Stop() {
    if (!is_running_.exchange(false)) {
        StopAudioThread();
        return;
    }
    StopAudioThread();
    if (realtime_client_) {
        realtime_client_->Close();
    }
    SetState(DialogState::kStopped);
    LOG_INFO("dialog session stopped");
}

DialogState DialogSession::State() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return state_;
}

void DialogSession::SetState(DialogState new_state) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    LOG_DEBUG("dialog state changed: {} -> {}",
              DialogStateToString(state_),
              DialogStateToString(new_state));
    state_ = new_state;
}

void DialogSession::OnInterviewStarted() {
    if (interview_session_ == nullptr) {
        LOG_WARN("interview session is nullptr");
        Stop();
        return;
    }

    SetState(DialogState::kInterviewerSpeaking);
    LOG_INFO("欢迎参加模拟面试");
    SetState(DialogState::kIdle);
}

void DialogSession::OnAskQuestion() {
    Question question = interview_session_->GetCurrentQuestion();
    SetState(DialogState::kInterviewerSpeaking);
    LOG_INFO("第{}题：{}", question.id, question.text);
}

void DialogSession::OnCandidateAnswer(const std::string& answer) {
    SetState(DialogState::kInterviewerThinking);
    EvaluateResult result = interview_session_->SubmitAnswer(answer);

    LOG_INFO("评分：{}", result.score);
    LOG_INFO("反馈：{}", result.feedback);

    if (result.need_followup) {
        SetState(DialogState::kInterviewerSpeaking);
        LOG_INFO("追问：{}", result.followup_question);
    }
}

void DialogSession::OnFollowupAnswer(const std::string& answer) {
    SetState(DialogState::kInterviewerThinking);

    EvaluateResult result = interview_session_->SubmitFollowupAnswer(answer);

    LOG_INFO("追问评分：{}", result.score);
    LOG_INFO("追问反馈：{}", result.feedback);
}

void DialogSession::OnEnterSummary() {
    SetState(DialogState::kSessionEnding);

    InterviewReport report = interview_session_->GenerateReport();
    SpeakText(report.summary);

    LOG_INFO("面试结束");
    LOG_INFO("总分：{}", report.total_score);
    LOG_INFO("总结：{}", report.summary);

    is_running_.store(false);
    StopAudioThread();
    SetState(DialogState::kCompleted);
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
    const nlohmann::json payload = {{"content", text}};
    std::string session_id;
    {
        std::lock_guard<std::mutex> lock(data_mutex_);
        session_id = session_id_;
    }
    if (!realtime_client_->SendEvent(events::kChatTextQuery, session_id_, payload)) {
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

    if (interview_session_->HasPendingFollowup()) {
        EvaluateResult result =
            interview_session_->SubmitFollowupAnswer(asr_text);
        LOG_INFO("追问评分：{}，反馈：{}", result.score, result.feedback);
        interview_session_->MoveToNextQuestion();
    } else {
        EvaluateResult result = interview_session_->SubmitAnswer(asr_text);
        LOG_INFO("评分：{}，反馈：{}", result.score, result.feedback);

        if (result.need_followup) {
            SetState(DialogState::kInterviewerSpeaking);
            LOG_INFO("追问：{}", result.followup_question);
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
        SpeakText(q.text);
    }
}

void DialogSession::OnServerEvent(const ParsedResponse& evt) {
    if (evt.code != 0) {
        LOG_ERROR("server error frame: code={}, json={}", evt.code, evt.payload_json);
        SetState(DialogState::kStopped);
        is_running_.store(false);
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
        if (interview_session_ && interview_session_->HasNextQuestion()) {
            Question q = interview_session_->GetCurrentQuestion();
            LOG_INFO("第{}题：{}", q.id, q.text);
            SpeakText(q.text);
        }
        break;

    case events::kTtsSentenceStart:
    case events::kTtsResponse:
        SetState(DialogState::kInterviewerSpeaking);
        if (evt.is_binary) {
            LOG_DEBUG("[event] TTS audio chunk bytes={}", evt.payload_bytes.size());
        }
        break;

    case events::kTtsEnded:
        SetState(DialogState::kIdle);
        break;

    case events::kAsrInfo:
        SetState(DialogState::kCandidateSpeaking);
        {
            std::lock_guard<std::mutex> lock(data_mutex_);
            current_asr_text_.clear();
        }

        break;

    case events::kAsrResult:
        {
            // payload_json 有两种来源,需要兼容:
            //   - Mock 路径(main/dialog_session_realtime_demo_main.cc 等):
            //     直接把裸 ASR 文本写到 payload_json,例如 "我熟悉 C++"。
            //   - Real 路径(豆包 SAMI 实时对话):
            //     payload_json 是服务端解 gzip 后的整段 JSON,
            //     真实文本在 "text" 或 "results[0].text" 字段里。
            // 先按 JSON 解析提取 text,失败或字段缺失再回退到把整段当裸文本,
            // 这样不动 RealtimeClient 公共接口也能同时兼容两条路径。
            std::string asr_text = ExtractAsrText(evt.payload_json);
            LOG_DEBUG("[event] kAsrResult text='{}', raw={}",
                      asr_text, evt.payload_json);
            std::lock_guard<std::mutex> lock(data_mutex_);
            // 常见实现为“当前整句覆盖”；若为增量需改为 +=
            current_asr_text_ = std::move(asr_text);
        }
        break;

    case events::kAsrEnded:
        HandleAsrFinalized();
        break;

    case events::kSessionFinished:
        LOG_INFO("[event] kSessionFinished");
        SetState(DialogState::kCompleted);
        is_running_.store(false);
        break;

    case events::kSessionFailed:
        LOG_ERROR("[event] kSessionFailed payload={}", evt.payload_json);
        SetState(DialogState::kStopped);
        is_running_.store(false);
        break;

    case events::kConnectionFailed:
        LOG_ERROR("[event] kConnectionFailed payload={}", evt.payload_json);
        SetState(DialogState::kStopped);
        is_running_.store(false);
        break;

    case events::kConnectionFinished:
        LOG_INFO("[event] kConnectionFinished");
        break;

    case events::kChatResponse:
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

void DialogSession::StartAudioThreads() {
    if (!audio_enabled_) {
        return;
    }
    if (audio_threads_running_.load()) {
        return;
    }

    audio_manager_ = std::make_unique<interview::services::AudioManager>();

    // 输入和输出任一端打不开都不能满足 “端到端语音”语义。
    // 这里降级为无音频并保留实时文本/TTS 事件链路，避免因为设备确实把整个会话对象留在半启动状态。
    if (!audio_manager_->StartInput() || !audio_manager_->StartOutput()) {
        LOG_ERROR("audio device startup failed; voice I/O disabled");
        audio_manager_->StopInput();
        audio_manager_->StopOutput();
        audio_manager_.reset();
        return;
    }

    audio_threads_running_.store(true);
    recording_thread_ = std::thread(&DialogSession::RecordingLoop, this);
    playback_thread_ = std::thread(&DialogSession::PlaybackLoop, this);
    LOG_INFO("dialog audio threads started");
}

void DialogSession::StopAudioThread() {
    if (!audio_threads_running_.exchange(false)) {
        return;
    }

    tts_cv_.notify_all();
    if (audio_manager_) {
        audio_manager_->StopInput();
        audio_manager_->StopOutput();
    }

    if (recording_thread_.joinable()) {
        if (recording_thread_.get_id() == std::this_thread::get_id()) {
            recording_thread_.detach();
        } else {
            recording_thread_.join();
        }
    }

    if (playback_thread_.joinable()) {
        if (playback_thread_.get_id() == std::this_thread::get_id()) {
            playback_thread_.detach();
        } else {
            playback_thread_.join();
        }
    }

    {
        std::lock_guard<std::mutex> lock(tts_mutex_);
        std::queue<std::vector<uint8_t>> empty;
        std::swap(tts_queue_, empty);
        tts_round_ended_ = false;
    }

    audio_manager_.reset();
    LOG_INFO("dialog audio threads stopped");
}


void DialogSession::RecordingLoop() {
    while (audio_threads_running_.load() && is_running_.load()) {
        if (!CanSendCandidateAudio()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            continue;
        }
        std::vector<uint8_t> pcm;
        if (!audio_manager_ || !audio_manager_->ReadInputChunk(pcm)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            continue;
        }

        const std::string session_id = SessionIdSnapshot();
        if (session_id.empty() || !realtime_client_ ||
            !realtime_client_->SendAudio(session_id, pcm)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }
    LOG_DEBUG("recording loop exited");
}

void DialogSession::PlaybackLoop() {
    while (audio_threads_running_.load()) {
        std::vector<uint8_t> pcm;
        {
            std::unique_lock<std::mutex> lock(tts_mutex_);
            tts_cv_.wait(lock, [this] {
                return !audio_threads_running_.load() || !tts_queue_.empty() ||
                        tts_round_ended_;
            });

            if (!audio_threads_running_.load()) {
                break;
            }
            if (tts_queue_.empty()) {
                tts_round_ended_ = false;
                continue;
            }
            
            pcm = std::move(tts_queue_.front());
            tts_queue_.pop();
        }
        if (audio_manager_ && !audio_manager_->PlayTtsPcm(pcm)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
    LOG_DEBUG("playback loop excited");
}

void DialogSession::EnqueueTtsAudio(const std::vector<uint8_t>& pcm) {
    if (!audio_threads_running_.load() || pcm.empty()) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(tts_mutex_);
        tts_queue_.push(pcm);
        tts_round_ended_ = false;
    }
    tts_cv_.notify_one();
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
