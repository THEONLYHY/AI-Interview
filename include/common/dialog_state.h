#ifndef COMMON_DIALOG_STATE_H
#define COMMON_DIALOG_STATE_H

enum class DialogState {
    kInit = 0,
    kConnecting,
    kInterviewerSpeaking,
    kIdle,
    kCandidateSpeaking,
    kInterviewerThinking,
    kSessionEnding,
    kCompleted,
    kStopped
};


inline std::string DialogStateToString(DialogState state) {
    switch (state) {
        case DialogState::kInit:
            return "kInit";
        case DialogState::kConnecting:
            return "kConnecting";
        case DialogState::kInterviewerSpeaking:
            return "kInterviewerSpeaking";
        case DialogState::kIdle:
            return "kIdle";
        case DialogState::kCandidateSpeaking:
            return "kCandidateSpeaking";
        case DialogState::kInterviewerThinking:
            return "kInterviewerThinking";
        case DialogState::kSessionEnding:
            return "kSessionEnding";
        case DialogState::kCompleted:
            return "kCompleted";
        case DialogState::kStopped:
            return "kStopped";
        default:
            return "UnknownState";
    }
}

#endif //COMMON_DIALOG_STATE_H