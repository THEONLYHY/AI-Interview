#include "interview/dialog_session.h"
#include "common/logger.h"

#include <iostream>

DialogSession::DialogSession(std::unique_ptr<InterviewSession> interview_session) 
: interview_session_(std::move(interview_session)){

}

void DialogSession::Start() {
    if (is_running_) {
        return;
    }
    is_running_ = true;
    SetState(DialogState::kConnecting);
    interview_session_->Start();
    OnInterviewStarted();
}

void DialogSession::Run() {
    if (!is_running_) {
        LOG_WARN("dialog session is not running, call Start() first");
        return;
    }
    while (is_running_ && interview_session_->HasNextQuestion()) {
        OnAskQuestion();

        SetState(DialogState::kCandidateSpeaking);

        std::string answer;
        std::getline(std::cin, answer);
        OnCandidateAnswer(answer);

        if (!is_running_) {
            break;
        }

        if (interview_session_->HasPendingFollowup()) {
            SetState(DialogState::kCandidateSpeaking);

            std::string followup_answer;
            std::getline(std::cin, followup_answer);

            OnFollowupAnswewr(answer);

            if (!is_running_) {
                break;
            }
        }
        interview_session_->MoveToNextQuestion();
        SetState(DialogState::kIdle);
    }

    if (is_running_ && state_ != DialogState::kStopped) {
        OnEnterSummary();
    }
}

void DialogSession::Stop() {
    if (!is_running_) {
        LOG_INFO("dialog session already stopped");
        SetState(DialogState::kStopped);
        return;
    }
    is_running_ = false;
    SetState(DialogState::kStopped);
    LOG_INFO("dialog session stopped");
}

DialogState DialogSession::state() const {
    return state_;
}

void DialogSession::SetState(DialogState new_state) {
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

void DialogSession::OnFollowupAnswewr(const std::string& answer) {
    SetState(DialogState::kInterviewerThinking);

    EvaluateResult result = interview_session_->SubmitFollowupAnswer(answer);

    LOG_INFO("追问评分：{}", result.score);
    LOG_INFO("追问反馈：{}", result.feedback);
}

void DialogSession::OnEnterSummary() {
    SetState(DialogState::kSessionEnding);

    InterviewReport report = interview_session_->GenerateReport();

    LOG_INFO("面试结束");
    LOG_INFO("总分：{}", report.total_score);
    LOG_INFO("总结：{}", report.summary);

    is_running_ = false;
    SetState(DialogState::kCompleted);
}
