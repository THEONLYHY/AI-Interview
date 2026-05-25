#ifndef INCLUDE_UI_SESSION_OPTIONS_H_
#define INCLUDE_UI_SESSION_OPTIONS_H_

#include <QMetaType>
#include <QString>

namespace interview::ui {

enum class LlmMode {
    kMock,
    kReal,
};

enum class RealtimeMode {
    kMockScript,
    kRealWss,
};

enum class InputMode {
    kTextMock,
    kVoice,
};

struct SessionOptions {
    QString candidate_name = "Candidate";
    QString resume_pdf_path;
    QString config_path = "config/local_config.json";
    int question_count = 3;
    LlmMode llm_mode = LlmMode::kMock;
    RealtimeMode realtime_mode = RealtimeMode::kMockScript;
    InputMode input_mode = InputMode::kTextMock;
};

}  // namespace interview::ui

Q_DECLARE_METATYPE(interview::ui::SessionOptions)

#endif  // INCLUDE_UI_SESSION_OPTIONS_H_
