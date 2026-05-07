#ifndef INTERVIEW_DIALOGSESSION_H
#define INTERVIEW_DIALOGSESSION_H

#include "interview_session.h"

#include <memory>

enum class DialogState {
    kInit = 0,
    kConnecting,
    kInterviewSpeaking,
    kIdle,
    kCandidateSpeaking,
    kInterviewThinking,
    kSessionEnding,
    kCompleted,
    kStopped
}


class DialogSession {
public:
    explicit DialogSession(std::unique_ptr<InterviewSession> interview_session);
    void Start();
    void Run();

    void Stop();

private:

private:
    std::unique_ptr<InterviewSession> interview_session_;
    DialogState state_ = DialogState::kInit;
    bool is_running_ = false;
};


#endif // INTERVIEW_DIALOGSESSION_H