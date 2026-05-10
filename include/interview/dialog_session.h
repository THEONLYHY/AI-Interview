#ifndef INTERVIEW_DIALOGSESSION_H
#define INTERVIEW_DIALOGSESSION_H

#include "interview_session.h"
#include "common/dialog_state.h"
#include "services/realtime_client.h"

#include <memory>
#include <string>

// DialogSession 是当前会话流程协调层。
// 负责：
// 1. 驱动 InterviewSession
// 2. 管理状态机
// 3. 持有 RealtimeClient
// 4. 接收实时层回调上来的消息

class DialogSession {
public:
    explicit DialogSession(std::unique_ptr<InterviewSession> interview_session,
                           std::unique_ptr<RealTimeClient> realtime_client);

    DialogSession(const DialogSession&) = delete;
    DialogSession& operator=(const DialogSession&) = delete;
    DialogSession(DialogSession&&) = delete;
    DialogSession& operator=(DialogSession&&) = delete;

    ~DialogSession() = default;

    void Start();
    void Run();
    void Stop();

    DialogState state() const;
private:
    void SetState(DialogState new_state);

    // 面试开始后的事件处理
    void OnInterviewStarted(); 
    // 负责出题
    void OnAskQuestion();
    // 处理主问题回答
    void OnCandidateAnswer(const std::string& answer);
    // 处理追问问答
    void OnFollowupAnswewr(const std::string& answer);
    // 进入总结阶段
    void OnEnterSummary();

      // 处理实时层回调上来的协议消息。
    void OnRealTimeMessage(const ProtocolMessage& message);

    // test
    void SendHelloMessage();
    void SimulateIncomingMessage();
private:
    std::unique_ptr<InterviewSession> interview_session_;
    std::unique_ptr<RealTimeClient> realtime_client_;

    DialogState state_ = DialogState::kInit;
    bool is_running_ = false;
};


#endif // INTERVIEW_DIALOGSESSION_H