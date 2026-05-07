#ifndef COMMON_DIALOG_STATE_H
#define COMMON_DIALOG_STATE_H

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
};


#endif //COMMON_DIALOG_STATE_H