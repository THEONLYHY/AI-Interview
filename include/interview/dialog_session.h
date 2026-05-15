#ifndef INTERVIEW_DIALOGSESSION_H
#define INTERVIEW_DIALOGSESSION_H

#include <atomic>
#include <memory>
#include <string>
#include <mutex>

#include "common/dialog_state.h"
#include "common/protocol.h"
#include "interview_session.h"
#include "services/realtime_client.h"

namespace interview::session {

// DialogSession：会话流程协调层
// 1. 驱动 InterviewSession（业务）
// 2. 管理 DialogState 状态机
// 3. 持有 RealtimeClient（Mock / Real）
// 4. OnServerEvent 接收 ParsedResponse（与 Protocol::ParseResponse 输出对齐）
class DialogSession {
public:
    explicit DialogSession(
        std::unique_ptr<InterviewSession> interview_session,
        std::unique_ptr<interview::services::RealtimeClient> realtime_client);

    DialogSession(const DialogSession&) = delete;
    DialogSession& operator=(const DialogSession&) = delete;
    DialogSession(DialogSession&&) = delete;
    DialogSession& operator=(DialogSession&&) = delete;

    ~DialogSession() = default;

    void Start();

    // stdin 文本问答（兼容旧入口，等价于 RunFromStdin）
    void Run();

    void RunFromStdin();
    // 阻塞直到 kSessionFinished / kStopped / Stop()
    void RunEventDriven();

    void Stop();

    interview::common::DialogState State() const;

private:
    void SetState(interview::common::DialogState new_state);

    // 面试开始后的事件处理
    void OnInterviewStarted();
    // 负责出题
    void OnAskQuestion();
    // 处理主问题回答
    void OnCandidateAnswer(const std::string& answer);
    void OnFollowupAnswer(const std::string& answer);
    void OnEnterSummary();

    void OnServerEvent(const interview::common::ParsedResponse& evt);

    // 通过 kChatTextQuery 让服务端 TTS 读文字（阶段 7 真链路透传）
    void SpeakText(const std::string& text);

    // kAsrEnded 时提交 accumulated ASR 文本并推进题流
    void HandleAsrFinalized();

private:
    std::unique_ptr<InterviewSession> interview_session_;
    std::unique_ptr<interview::services::RealtimeClient> realtime_client_;


    interview::common::DialogState state_ = interview::common::DialogState::kInit;
    mutable std::mutex state_mutex_;    

    // data_mutex_ 保护服务端事件携带的会话数据。
    // session_id_ 在 kSessionStarted 中写入，SpeakText/SendAudio 等发送路径读取；
    // current_asr_text_ 由 ASR 事件持续覆盖，kAsrEnded 时提交给 InterviewSession。
    mutable std::mutex data_mutex_;

    // OnServerEvent 在工作线程、Run*/Stop 在主线程，需原子避免 data race
    std::atomic<bool> is_running_{false};

    std::string session_id_;
    std::string current_asr_text_;
};

}  // namespace interview::session

#endif  // INTERVIEW_DIALOGSESSION_H
