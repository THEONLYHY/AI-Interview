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

// interview_types 提供 Question、EvaluateResult、InterviewReport 等业务结构。
#include "common/interview_types.h"
#include "common/logger.h"

#include "nlohmann/json.hpp"

namespace interview::session {

// 下面的 using 缩短本文件中高频出现的 common 类型名。
using interview::common::DialogState;
using interview::common::DialogStateToString;
using interview::common::EvaluateResult;
using interview::common::InterviewReport;
using interview::common::MessageType;
using interview::common::ParsedResponse;
using interview::common::Question;
// events 命名空间集中保存服务端事件 id 常量。
namespace events = interview::common::events;
// RealtimeClient 是 DialogSession 与传输层之间的抽象边界。
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
    // 空 payload 没有可提交的候选人文本。
    if (payload_json.empty()) {
        return {};
    }
    // Mock 路径直接把裸文本放在 payload_json 中，不需要 JSON 解析。
    if (payload_json.front() != '{' && payload_json.front() != '[') {
        return payload_json;
    }
    try {
        // 真实链路下 payload_json 是服务端返回的 JSON 字符串。
        const auto j = nlohmann::json::parse(payload_json);
        if (j.is_object()) {
            // 优先读取最直接的 text 字段。
            if (auto it = j.find("text");
                it != j.end() && it->is_string()) {
                return it->get<std::string>();
            }
            // 部分 ASR 返回把文本放在 results[0].text。
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
        // JSON 合法但没有文本字段时，上层会把空串视为“无需提交”。
        return {};
    } catch (const nlohmann::json::exception&) {
        // JSON 解析失败时保守回退为裸文本，避免中断语音流程。
        return payload_json;
    }
}

std::vector<uint8_t> Pcm16SamplesToLeBytes(
    const std::vector<int16_t>& samples) {
    // 实时客户端发送的是字节流，因此先为每个 int16 预留两个字节。
    std::vector<uint8_t> bytes;
    bytes.reserve(samples.size() * sizeof(int16_t));
    for (const int16_t sample : samples) {
        // int16_t 转 uint16_t 后按小端顺序拆成低字节、高字节。
        const auto raw = static_cast<uint16_t>(sample);
        bytes.push_back(static_cast<uint8_t>(raw & 0xFF));
        bytes.push_back(static_cast<uint8_t>((raw >> 8) & 0xFF));
    }
    // 返回值保持 pcm_s16le 排列，供 SendAudio 直接发送。
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
    // 已经运行时直接返回，避免重复连接和重复启动音频线程。
    if (is_running_.load()) {
        return;
    }
    if (interview_session_ == nullptr) {
        LOG_ERROR("interview session is nullptr");
        EmitContent("error", "interview session is nullptr", -1);
        SetState(DialogState::kStopped);
        return;
    }

    // 标记会话运行，并先进入连接中状态。
    is_running_.store(true);
    SetState(DialogState::kConnecting);
    // 启动业务会话，通常会重置题目游标和内部评分状态。
    interview_session_->Start();

    LOG_INFO("欢迎参加模拟面试");
    EmitContent("system", "欢迎参加模拟面试", -1);

    // 有 realtime_client_ 时走事件驱动链路；没有时保留纯文本路径。
    if (realtime_client_) {
        // 回调必须先于 Connect，避免 Mock 线程瞬间投递丢失首包。
        // 先注册事件回调，再连接，避免 Mock/真实服务端首包早于订阅。
        realtime_client_->SetEventHandler(
            [this](const ParsedResponse& evt) { OnServerEvent(evt); });

        // 连接失败时立即收敛到停止状态，避免后续发送使用未连接客户端。
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

        // 语音模式需要先准备本地输入/输出设备，再请求服务端朗读题目。
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

            // 给服务端 session started / 音频设备启动留出短暂缓冲。
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            // 发送当前题目文本，让服务端开始生成 TTS。
            RequestCurrentQuestionSpeech();
        }
    } else {
        // 没有实时客户端时，Start() 只完成业务初始化并停在空闲状态。
        SetState(DialogState::kIdle);
    }
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

    // 先停本地音频线程，避免关闭连接后录音线程继续 SendAudio。
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
    // state_ 被多线程读写，读取时必须持锁。
    std::lock_guard<std::mutex> lock(state_mutex_);
    return state_;
}

void DialogSession::SetContentCallback(DialogContentCallback callback) {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    content_callback_ = std::move(callback);
}

void DialogSession::SetStateCallback(DialogStateCallback callback) {
    // 注册时需要把当前状态立即回放给新订阅者。
    DialogState current_state;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        current_state = state_;
    }

    DialogStateCallback cb;
    {
        // 复制一份回调到局部变量，后续在锁外调用。
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
    // 先在状态锁内完成旧状态记录和新状态写入。
    DialogState old_state;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        // 重复状态不广播
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
        // 复制回调，
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
    // 空文本没有 UI 展示意义，也避免测试收到空事件。
    if (text.empty()) {
        return;
    }

    DialogContentCallback cb;
    {
        // 复制回调后立刻释放锁，避免 UI 回调反向调用造成死锁。
        std::lock_guard<std::mutex> lock(callback_mutex_);
        cb = content_callback_;
    }

    // 回调可能由后台线程触发；业务层不关心 UI 线程切换。
    if (cb) {
        cb(role, text, question_index);
    }
}

void DialogSession::OnAskQuestion() {
    // 从业务会话读取当前题目快照。
    Question question = interview_session_->GetCurrentQuestion();
    // 面试官开始展示/朗读题目。
    SetState(DialogState::kInterviewerSpeaking);
    LOG_INFO("第{}题：{}", question.id, question.text);
    // 通过统一内容回调交给 UI 或测试观察。
    EmitContent("question", question.text, question.id);
}

void DialogSession::OnCandidateAnswer(const std::string& answer) {
    const Question question = interview_session_->GetCurrentQuestion();
    // 先展示候选人回答，再进入评分阶段。
    EmitContent("candidate", answer, question.id);

    SetState(DialogState::kInterviewerThinking);
    // SubmitAnswer 会更新 InterviewSession 内部评分和追问状态。
    EvaluateResult result = interview_session_->SubmitAnswer(answer);

    LOG_INFO("评分：{}", result.score);
    LOG_INFO("反馈：{}", result.feedback);
    // 反馈内容合并成一条事件，便于 UI 作为同一段反馈展示。
    EmitContent("feedback",
                "评分：" + std::to_string(result.score) +
                    "\n反馈：" + result.feedback,
                question.id);

    // 如果需要追问，只展示追问文本；回答读取由调用方继续处理。
    if (result.need_followup) {
        SetState(DialogState::kInterviewerSpeaking);
        LOG_INFO("追问：{}", result.followup_question);
        EmitContent("followup", result.followup_question, question.id);
    }
}

void DialogSession::OnFollowupAnswer(const std::string& answer) {
    // 追问题目保存在 InterviewSession 的 pending follow-up 槽中。
    const Question question = interview_session_->GetPendingFollowupQuestion();
    // UI 需要按父问题归类追问回答，缺失父 id 时回退到自身 id。
    const int question_id = question.parent_question_id >= 0
                                ? question.parent_question_id
                                : question.id;
    // 展示候选人的追问回答。
    EmitContent("candidate", answer, question_id);

    SetState(DialogState::kInterviewerThinking);

    // SubmitFollowupAnswer 会清理 pending follow-up 并返回追问评分。
    EvaluateResult result = interview_session_->SubmitFollowupAnswer(answer);

    LOG_INFO("追问评分：{}", result.score);
    LOG_INFO("追问反馈：{}", result.feedback);
    // 追问反馈同样以一条 feedback 内容事件发出。
    EmitContent("feedback",
                "追问评分：" + std::to_string(result.score) +
                    "\n追问反馈：" + result.feedback,
                question_id);
}

void DialogSession::OnEnterSummary() {
    // 总结阶段表示不再接受新的候选人回答。
    SetState(DialogState::kSessionEnding);

    // GenerateReport 聚合整场面试的评分和总结文本。
    InterviewReport report = interview_session_->GenerateReport();
    // 语音/实时模式下请求服务端朗读总结；无实时客户端时会自动跳过。
    SpeakText(report.summary);

    LOG_INFO("面试结束");
    LOG_INFO("总分：{}", report.total_score);
    LOG_INFO("总结：{}", report.summary);
    // UI 使用 summary role 展示最终结果。
    EmitContent("summary",
                "总分：" + std::to_string(report.total_score) +
                    "\n总结：" + report.summary,
                -1);

    // 标记完成并让外层事件循环退出。
    SetState(DialogState::kCompleted);
    is_running_.store(false);
}

void DialogSession::RequestCurrentQuestionSpeech() {
    // 没有业务会话或没有剩余题目时无需请求 TTS。
    if (!interview_session_ || !interview_session_->HasNextQuestion()) {
        return;
    }

    // 读取当前题目，先让 UI 看到文字，再触发服务端朗读。
    Question q = interview_session_->GetCurrentQuestion();
    LOG_INFO("第{}题：{}", q.id, q.text);
    EmitContent("question", q.text, q.id);
    // 这里只是发送文本请求，让服务端开始生成/下发 TTS。
    // 本地播放必须等后续 kTtsResponse(ServerAck/binary) 到达后才发生。
    SpeakText(q.text);
}

void DialogSession::SpeakText(const std::string& text) {
    // 空文本不请求 TTS，避免服务端收到无效朗读任务。
    if (text.empty()) {
        return;
    }
    // 没有实时客户端时只记录调试日志，文本模式无需朗读。
    if (!realtime_client_) {
        LOG_DEBUG("[no realtime] TTS 跳过：{}", text);
        return;
    }
    // 豆包 ChatTextQuery：具体字段以接入时官方文档为准。
    // 这里先拷贝 session_id_ 再发送，避免在网络 write 期间持有 data_mutex_。
    const nlohmann::json payload =
        interview::common::Protocol::BuildReadAloudTextQueryPayload(text);
    // 发送前复制 session_id，避免网络写入期间持有 data_mutex_。
    std::string session_id;
    {
        std::lock_guard<std::mutex> lock(data_mutex_);
        session_id = session_id_;
    }
    // 一旦请求朗读，就先进入播放门控，直到本地播放确认 drain。
    is_playing_audio_.store(true);
    SetState(DialogState::kInterviewerSpeaking);
    // 唤醒播放线程重新检查状态；真正播放仍等 TTS 音频入队。
    tts_cv_.notify_all();
    // 发送 ChatTextQuery 让服务端开始生成并下发 TTS。
    if (!realtime_client_->SendEvent(events::kChatTextQuery, session_id, payload)) {
        LOG_WARN("SendEvent(kChatTextQuery) failed");
    }
}

void DialogSession::HandleAsrFinalized() {
    // 会话已停止时清空残留 ASR，避免下一次启动复用旧文本。
    if (!is_running_.load()) {
        std::lock_guard<std::mutex> lock(data_mutex_);
        current_asr_text_.clear();
        return;
    }
    // ASR 结束后进入面试官思考/评分阶段。
    SetState(DialogState::kInterviewerThinking);

    std::string asr_text;
    {
        // ASR 文本快照只在锁内复制和清空。后续 LLM 评分可能耗时，
        // 不能在持锁状态下调用 InterviewSession。
        std::lock_guard<std::mutex> lock(data_mutex_);
        asr_text = current_asr_text_;
        current_asr_text_.clear();
    }
    // 没有识别出有效文本时不提交 InterviewSession。
    if (asr_text.empty()) {
        LOG_WARN("kAsrEnded but ASR text empty, skip submit");
        return;
    }

    LOG_INFO("候选人回答（ASR）：{}", asr_text);
    // 当前题目 id 用于 UI 归档；如果题目已结束则使用 -1。
    const int question_id = interview_session_->HasNextQuestion()
                                ? interview_session_->GetCurrentQuestion().id
                                : -1;
    // 先显示候选人识别文本，再计算评分。
    EmitContent("candidate", asr_text, question_id);

    // pending follow-up 优先，说明这段 ASR 属于追问回答。
    if (interview_session_->HasPendingFollowup()) {
        EvaluateResult result =
            interview_session_->SubmitFollowupAnswer(asr_text);
        LOG_INFO("追问评分：{}，反馈：{}", result.score, result.feedback);
        EmitContent("feedback",
                    "追问评分：" + std::to_string(result.score) +
                        "\n追问反馈：" + result.feedback,
                    question_id);
        // 追问结束后当前主问题完整结束，推进到下一题。
        interview_session_->MoveToNextQuestion();
    } else {
        // 普通主回答路径，可能生成追问。
        EvaluateResult result = interview_session_->SubmitAnswer(asr_text);
        LOG_INFO("评分：{}，反馈：{}", result.score, result.feedback);
        EmitContent("feedback",
                    "评分：" + std::to_string(result.score) +
                        "\n反馈：" + result.feedback,
                    question_id);

        // 有追问时立即朗读追问，不推进题目游标。
        if (result.need_followup) {
            SetState(DialogState::kInterviewerSpeaking);
            LOG_INFO("追问：{}", result.followup_question);
            EmitContent("followup", result.followup_question, question_id);
            SpeakText(result.followup_question);
            return;
        }
        // 无追问则当前题结束，进入下一题或总结。
        interview_session_->MoveToNextQuestion();
    }

    // 所有题都完成时进入总结。
    if (!interview_session_->HasNextQuestion()) {
        OnEnterSummary();
    } else {
        // 还有题目时展示并朗读下一题。
        Question q = interview_session_->GetCurrentQuestion();
        LOG_INFO("第{}题：{}", q.id, q.text);
        EmitContent("question", q.text, q.id);
        SpeakText(q.text);
    }
}

void DialogSession::OnServerEvent(const ParsedResponse& evt) {
    // code 非 0 或服务端错误帧都视为会话级错误。
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
    // 回退保护，防止迟到的 TTS/ASR/普通业务事件回写终态后的状态
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
    // 按协议事件 id 分发，每个分支只处理自己关心的状态和数据。
    switch (evt.event) {
        case events::kConnectionStarted:
            // 连接建立通知只记录 connect_id，不推进题目流程。
            LOG_INFO("[event] kConnectionStarted connect_id={}", evt.connect_id);
            break;

        case events::kSessionStarted:
            // session_id 是后续文本请求和音频帧发送的必要字段。
            LOG_INFO("[event] kSessionStarted session_id={}", evt.session_id);
            {
                std::lock_guard<std::mutex> lock(data_mutex_);
                session_id_ = evt.session_id;
            }

            // 会话建立后进入空闲状态；非语音模式可以直接请求朗读第一题。
            SetState(DialogState::kIdle);
            if (!audio_enabled_) {
                RequestCurrentQuestionSpeech();
            }
            break;

        case events::kTtsSentenceStart:
            // 服务端开始生成某句 TTS，此时先关闭候选人真实麦克风上行。
            LOG_INFO("[event] kTtsSentenceStart");
            is_playing_audio_.store(true);
            {
                std::lock_guard<std::mutex> lock(tts_mutex_);
                // 新一轮 TTS 开始，清除上一轮 TTS_END 标记。
                tts_end_received_ = false;
            }
            SetState(DialogState::kInterviewerSpeaking);
            break;

        case events::kTtsResponse:
            // TTS 音频只接受 ServerAck + binary；JSON ack 不进入播放队列。
            if (evt.message_type != MessageType::kServerAck || !evt.is_binary) {
                LOG_WARN("[event] ignored non-binary TTS response: message_type={}, json={}",
                        static_cast<int>(evt.message_type), evt.payload_json);
                break;
            }
            // 收到音频 chunk 后保持面试官说话状态，并入队给播放线程。
            is_playing_audio_.store(true);
            SetState(DialogState::kInterviewerSpeaking);
            LOG_DEBUG("[event] TTS audio chunk bytes={}", evt.payload_bytes.size());
            EnqueueTtsAudio(evt.payload_bytes);
            break;

        case events::kTtsEnded:
            // 服务端 TTS_END 只说明服务端发完；本地还要等待队列播放完。
            LOG_INFO("[event] kTtsEnded");
            {
                std::lock_guard<std::mutex> lock(tts_mutex_);
                tts_end_received_ = true;
                if (!tts_decode_remainder_.empty()) {
                    LOG_WARN("discarding incomplete TTS float32 tail bytes={}",
                            tts_decode_remainder_.size());
                    tts_decode_remainder_.clear();
                }
            }
            // 唤醒播放线程并尝试判断本地播放是否已经 drain。
            tts_cv_.notify_all();
            MarkTtsPlaybackDrained();
            break;

        case events::kAsrInfo:
            // 如果本地仍在播放 TTS，ASR start 属于回声窗口，直接忽略。
            if (State() == DialogState::kInterviewerSpeaking) {
                LOG_DEBUG("[event] ignored ASR start during TTS playback");
                break;
            }
            // 真正进入候选人说话阶段，并清理上一段 ASR 暂存。
            SetState(DialogState::kCandidateSpeaking);
            {
                std::lock_guard<std::mutex> lock(data_mutex_);
                current_asr_text_.clear();
            }

            break;

        case events::kAsrResult:
            {
                // 每个 ASR_RESULT 刷新最新识别文本，最终以 kAsrEnded 为准提交。
                std::string asr_text = ExtractAsrText(evt.payload_json);
                LOG_DEBUG("[event] kAsrResult text='{}', raw={}",
                        asr_text, evt.payload_json);
                std::lock_guard<std::mutex> lock(data_mutex_);
                current_asr_text_ = std::move(asr_text);
            }
            break;

        case events::kAsrEnded:
            // TTS 播放时到来的 ASR end 同样视为回声/噪声，不推进题目。
            if (State() == DialogState::kInterviewerSpeaking) {
                LOG_DEBUG("[event] ignored ASR end during TTS playback");
                break;
            }
            // 候选人语音结束，提交当前 ASR 文本给业务会话。
            HandleAsrFinalized();
            break;

        case events::kSessionFinished:
            // 服务端明确结束会话，停止本地音频线程并让外层循环退出。
            LOG_INFO("[event] kSessionFinished");
            SetState(DialogState::kCompleted);
            is_running_.store(false);
            StopAudioThread();
            break;

        case events::kSessionFailed:
            // 会话失败是终止性错误，需通知 UI 并停止音频。
            LOG_ERROR("[event] kSessionFailed payload={}", evt.payload_json);
            EmitContent("error", "session failed: " + evt.payload_json, -1);
            SetState(DialogState::kStopped);
            is_running_.store(false);
            StopAudioThread();
            break;

        case events::kConnectionFailed:
            // 连接失败通常来自传输层，处理方式与会话失败一致。
            LOG_ERROR("[event] kConnectionFailed payload={}", evt.payload_json);
            EmitContent("error", "connection failed: " + evt.payload_json, -1);
            SetState(DialogState::kStopped);
            is_running_.store(false);
            StopAudioThread();
            break;

        case events::kConnectionFinished:
            // 连接正常结束只记录日志，终态由 session finished/Stop 决定。
            LOG_INFO("[event] kConnectionFinished");
            break;

        case events::kChatResponse:
        case events::kChatQuestionInfo:
        case events::kChatEnded:
        case events::kTtsSentenceEnd:
            // 这些事件暂不改变本地状态，保留 DEBUG 日志便于后续排查协议。
            LOG_DEBUG("[event] passthrough id={}", evt.event);
            break;

        case events::kUsage:
            // 服务端在每次大模型响应结束后下发 token 计费统计。
            // 当前不做累计，只在 DEBUG 级别落盘，避免 default 分支误报 WARN。
            LOG_DEBUG("[event] kUsage json={}", evt.payload_json);
            break;

        default:
            // 未识别事件按 binary/json 分开记录，避免二进制 payload 打入文本日志。
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
    // 非语音模式不需要任何 PortAudio 资源，直接视为成功。
    if (!audio_enabled_) {
        return true;
    }
    // 已经启动时保持幂等。
    if (audio_threads_running_.load()) {
        return true;
    }

    // AudioManager 封装 PortAudio 输入/输出流。
    audio_manager_ = std::make_unique<interview::services::AudioManager>();

    // 先打开输入，再打开输出；任一失败都在 AudioManager 边界转成 false。
    if (!audio_manager_->OpenStreams()) {
        audio_manager_.reset();
        return false;
    }

    // 流打开成功后再启动线程，避免线程看到半初始化的 audio_manager_。
    audio_threads_running_.store(true);
    recording_thread_ = std::thread(&DialogSession::RecordingLoop, this);
    playback_thread_ = std::thread(&DialogSession::PlaybackLoop, this);
    LOG_INFO("dialog audio threads started");
    return true;
}

void DialogSession::StopAudioThread() {
    // Stop()、错误事件、SessionFinished 都可能调用这里，用 stop_mutex_ 串行化。
    std::lock_guard<std::mutex> lock(stop_mutex_);

    // exchange(false) 通知两个音频线程退出，并记录之前是否真的在运行。
    const bool was_running = audio_threads_running_.exchange(false);

    // 唤醒可能阻塞在条件变量上的播放线程。
    tts_cv_.notify_all();
    if (audio_manager_) {
        // 停止 PortAudio 流会帮助 ReadAudio/WriteAudio 尽快返回。
        audio_manager_->StopInput();
        audio_manager_->StopOutput();
    }

    // 等待录音线程退出，避免销毁 audio_manager_ 后仍读麦克风。
    if (recording_thread_.joinable()) {
        recording_thread_.join();
    }

    // 等待播放线程退出，避免销毁 audio_manager_ 后仍写扬声器。
    if (playback_thread_.joinable()) {
        playback_thread_.join();
    }

    {
        // 清空所有 TTS 状态，确保下一次启动不会继承旧音频。
        std::lock_guard<std::mutex> lock(tts_mutex_);
        std::queue<std::vector<float>> empty;
        std::swap(tts_queue_, empty);
        tts_decode_remainder_.clear();
        tts_playback_in_flight_ = 0;
        tts_end_received_ = false;
    }
    // 停止后录音门控也恢复为非播放状态。
    is_playing_audio_.store(false);

    // 释放 PortAudio 封装对象。
    audio_manager_.reset();
    if (was_running) {
        LOG_INFO("dialog audio threads stopped");
    }
}


void DialogSession::RecordingLoop() {
    // 打开该环境变量后，每秒输出一次输入音量和当前状态。
    const bool audio_debug = std::getenv("AI_INTERVIEW_AUDIO_DEBUG") != nullptr;
    auto next_audio_debug_log = std::chrono::steady_clock::now();

    // 录音线程同时受音频线程标志和会话运行标志控制。
    while (audio_threads_running_.load() && is_running_.load()) {
        // 理论上启动线程前 audio_manager_ 已存在；这里防御 Stop 并发。
        if (!audio_manager_) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            continue;
        }
        // 阻塞读取一块 int16 麦克风样本。
        const std::vector<int16_t> samples = audio_manager_->ReadAudio();
        if (samples.empty()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            continue;
        }
        // 实时协议发送 pcm_s16le bytes，而不是 int16 vector。
        std::vector<uint8_t> pcm = Pcm16SamplesToLeBytes(samples);

        // 播放 TTS 时启用回声门控：仍发送帧，但内容填静音。
        const bool is_playing_audio = is_playing_audio_.load();
        // 先计算真实输入音量，调试日志才能看到麦克风原始状态。
        const auto input_level =
            interview::services::AudioManager::Pcm16LeLevel(pcm);

        if (is_playing_audio) {
            // 把本帧清零，避免服务端 ASR 把本地扬声器声音识别成候选人回答。
            std::fill(pcm.begin(), pcm.end(), 0);
        }

        // 发送前取 session id 快照，避免长时间持有 data_mutex_。
        const std::string session_id = SessionIdSnapshot();
        if (session_id.empty() || !realtime_client_ ||
            !realtime_client_->IsConnected()) {
            // 会话尚未建立或连接断开时不发送音频，短暂休眠后重试。
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            continue;
        }

        // SendAudio 失败通常表示暂时不可写，休眠后由下一轮重试。
        if (!realtime_client_->SendAudio(session_id, pcm)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }

        // 调试日志限频到 1 秒一次，避免音频循环刷爆日志。
        if (audio_debug) {
            const auto now = std::chrono::steady_clock::now();
            if (now >= next_audio_debug_log) {
                LOG_INFO("[audio-debug] mic mode={} peak={} rms={:.1f} bytes={} state={}",
                         is_playing_audio ? "silence" : "real",
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
    // 每个播放块结束后都需要减少 in-flight 计数并检查是否 drain。
    const auto finish_playback_chunk = [this] {
        {
            std::lock_guard<std::mutex> lock(tts_mutex_);
            if (tts_playback_in_flight_ > 0) {
                --tts_playback_in_flight_;
            }
        }
        MarkTtsPlaybackDrained();
    };

    // 播放线程只由 audio_threads_running_ 控制，StopAudioThread() 会负责置 false。
    while (audio_threads_running_.load()) {
        std::vector<float> samples;
        {
            std::unique_lock<std::mutex> lock(tts_mutex_);
            // 等待新 TTS 样本入队，或等待停止信号。
            tts_cv_.wait(lock, [this] {
                return !audio_threads_running_.load() || !tts_queue_.empty();
            });

            if (!audio_threads_running_.load()) {
                break;
            }
            // 出队后标记一个播放块正在写入 PortAudio。
            samples = std::move(tts_queue_.front());
            tts_queue_.pop();
            ++tts_playback_in_flight_;
        }

        // Stop 过程中 audio_manager_ 可能已被清理，防御性跳过当前块。
        if (!audio_manager_) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            finish_playback_chunk();
            continue;
        }
        try {
            // WriteAudio 是阻塞写入，返回时该块已交给本地输出流。
            audio_manager_->WriteAudio(samples);
        } catch (const std::exception& e) {
            // 单块播放失败不直接杀会话，记录后继续处理后续块/停止信号。
            LOG_WARN("write output stream failed: {}", e.what());
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        finish_playback_chunk();
    }
    LOG_DEBUG("playback loop exited");
}

void DialogSession::MarkTtsPlaybackDrained() {
    // 终态下不再推进到 Idle，避免完成/停止后状态回退。
    const DialogState state = State();
    if (state == DialogState::kCompleted || state == DialogState::kStopped) {
        return;
    }
    {
        // 同时满足服务端 TTS_END、队列为空、无写入中块，才算本地播放结束。
        std::lock_guard<std::mutex> lock(tts_mutex_);
        if (!tts_end_received_ || !tts_queue_.empty() ||
            tts_playback_in_flight_ > 0) {
            return;
        }
        // 消费本轮 TTS_END 标记，避免重复触发 drain。
        tts_end_received_ = false;
    }

    // 解除录音静音门控。
    is_playing_audio_.store(false);
    if (state == DialogState::kInterviewerSpeaking) {
        // TTS 真正播完后，才让候选人开始说话/让 ASR 事件生效。
        SetState(DialogState::kIdle);
        LOG_INFO("TTS playback drained; candidate microphone audio enabled");
    }
}

void DialogSession::EnqueueTtsAudio(const std::vector<uint8_t>& pcm) {
    // 未启动音频线程或空包都没有播放意义。
    if (!audio_threads_running_.load() || pcm.empty()) {
        return;
    }

    // queued 标记用于在锁外唤醒播放线程。
    bool queued = false;
    {
        std::lock_guard<std::mutex> lock(tts_mutex_);
        // 服务端 chunk 可能不是 4 字节对齐，先拼接到 remainder。
        tts_decode_remainder_.insert(tts_decode_remainder_.end(),
                                     pcm.begin(), pcm.end());

        // 只解码完整的 float32 字节，残留尾巴留给下一包。
        const std::size_t complete_bytes =
            (tts_decode_remainder_.size() / sizeof(float)) * sizeof(float);
        if (complete_bytes > 0) {
            // 拷贝完整部分供解码，再从 remainder 中擦除。
            const std::vector<uint8_t> complete(
                tts_decode_remainder_.begin(),
                tts_decode_remainder_.begin() + complete_bytes);
            std::vector<float> samples =
                interview::services::AudioManager::Float32PcmLeToSamples(complete);
            tts_decode_remainder_.erase(
                tts_decode_remainder_.begin(),
                tts_decode_remainder_.begin() + complete_bytes);

            if (!samples.empty()) {
                // 解码成功的样本入队，由 PlaybackLoop 串行写入输出流。
                tts_queue_.push(std::move(samples));
                queued = true;
            }
        }
    }

    if (queued) {
        // 只在确实入队后唤醒，减少播放线程无意义 wakeup。
        tts_cv_.notify_one();
    }
}

std::string DialogSession::SessionIdSnapshot() const {
    // session_id_ 由 kSessionStarted 写入，录音线程高频读取。
    std::lock_guard<std::mutex> lock(data_mutex_);
    return session_id_;
}


}  // namespace interview::session
