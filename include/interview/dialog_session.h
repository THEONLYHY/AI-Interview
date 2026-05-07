#ifndef INTERVIEW_DIALOGSESSION_H
#define INTERVIEW_DIALOGSESSION_H

#include "interview_session.h"
#include "common/dialog_state.h"

#include <memory>
#include <string>


class DialogSession {
public:
    explicit DialogSession(std::unique_ptr<InterviewSession> interview_session);

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

private:
    std::unique_ptr<InterviewSession> interview_session_;
    DialogState state_ = DialogState::kInit;
    bool is_running_ = false;
};


#endif // INTERVIEW_DIALOGSESSION_H