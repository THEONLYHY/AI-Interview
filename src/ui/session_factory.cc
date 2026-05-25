#include "ui/session_factory.h"

#include <chrono>
#include <exception>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "common/config.h"
#include "common/protocol.h"
#include "interview/interview_session.h"
#include "services/mock_llm_client.h"
#include "services/mock_realtime_client.h"
#include "services/pdf_parser.h"
#include "services/real_llm_client.h"
#include "services/real_realtime_client.h"

namespace interview::ui {
namespace {

interview::common::ParsedResponse MakeEvent(uint32_t event_id,
                                            std::string session_id = {},
                                            std::string payload_json = {}) {
    interview::common::ParsedResponse event;
    event.event = event_id;
    event.session_id = std::move(session_id);
    event.payload_json = std::move(payload_json);
    return event;
}

std::vector<interview::common::ParsedResponse> BuildMockScript(
    const std::string& session_id,
    int question_count) {
    namespace events = interview::common::events;

    std::vector<interview::common::ParsedResponse> script;
    script.push_back(MakeEvent(events::kConnectionStarted));
    script.back().connect_id = "qt-mock-connect";
    script.push_back(MakeEvent(events::kSessionStarted, session_id));

    const char* answers[] = {
        "I use RAII to bind resource lifetime to object lifetime.",
        "Smart pointers express ownership and release memory automatically.",
        "Epoll scales better because it reports ready descriptors directly.",
        "A worker queue can distribute accepted sockets to event loops.",
        "Channel stores fd interests and dispatches callbacks in an EventLoop.",
        "Cleanup should unregister channels before closing descriptors.",
    };

    const int reusable_answers =
        static_cast<int>(sizeof(answers) / sizeof(answers[0]));
    for (int i = 0; i < question_count; ++i) {
        const char* answer = answers[i % reusable_answers];
        script.push_back(MakeEvent(events::kTtsEnded, session_id));
        script.push_back(MakeEvent(events::kAsrInfo, session_id));
        script.push_back(MakeEvent(events::kAsrResult, session_id, answer));
        script.push_back(MakeEvent(events::kAsrEnded, session_id));
    }
    script.push_back(MakeEvent(events::kSessionFinished, session_id));
    return script;
}

bool NeedsConfig(const SessionOptions& options) {
    return options.llm_mode == LlmMode::kReal ||
           options.realtime_mode == RealtimeMode::kRealWss ||
           options.input_mode == InputMode::kVoice;
}

SessionFactoryResult ErrorResult(QString message) {
    SessionFactoryResult result;
    result.error = std::move(message);
    return result;
}

}  // namespace

SessionFactoryResult SessionFactory::Create(const SessionOptions& options) {
    try {
        interview::common::AppConfig config;
        if (NeedsConfig(options)) {
            const std::string path = options.config_path.toStdString();
            if (!interview::common::Config::Instance().LoadFromFile(path)) {
                return ErrorResult(
                    QStringLiteral("Failed to load config file: %1")
                        .arg(options.config_path));
            }
            config = interview::common::Config::Instance().Snapshot();
        }

        std::string resume_text;
        if (!options.resume_pdf_path.isEmpty()) {
            const std::string resume_path = options.resume_pdf_path.toStdString();
            interview::services::PDFParser parser;
            if (!parser.IsValidPDF(resume_path)) {
                return ErrorResult(
                    QStringLiteral("Resume PDF is not valid: %1")
                        .arg(options.resume_pdf_path));
            }
            resume_text = parser.ExtractText(resume_path);
            if (resume_text.empty()) {
                return ErrorResult(
                    QStringLiteral("Failed to extract resume text from: %1")
                        .arg(options.resume_pdf_path));
            }
        }

        std::unique_ptr<interview::services::LLMClient> llm_client;
        if (options.llm_mode == LlmMode::kReal) {
            llm_client = std::make_unique<interview::services::RealLLMClient>(
                config.llm.api_url, config.llm.api_key, config.llm.model,
                config.llm.temperature, config.llm.max_tokens,
                config.llm.timeout_seconds);
        } else {
            llm_client =
                std::make_unique<interview::services::MockLLMClient>();
        }

        std::unique_ptr<interview::services::RealtimeClient> realtime_client;
        if (options.realtime_mode == RealtimeMode::kRealWss) {
            realtime_client =
                std::make_unique<interview::services::RealRealtimeClient>(
                    config.ws.base_url);
        } else {
            realtime_client =
                std::make_unique<interview::services::MockRealtimeClient>(
                    BuildMockScript("qt-mock-session", options.question_count),
                    std::chrono::milliseconds(250));
        }

        auto interview_session =
            std::make_unique<interview::session::InterviewSession>(
                std::move(llm_client), std::move(resume_text),
                options.question_count);
        auto dialog = std::make_unique<interview::session::DialogSession>(
            std::move(interview_session), std::move(realtime_client),
            options.input_mode == InputMode::kVoice);

        SessionFactoryResult result;
        result.session = std::move(dialog);
        return result;
    } catch (const std::exception& e) {
        return ErrorResult(QStringLiteral("Failed to create session: %1")
                               .arg(QString::fromUtf8(e.what())));
    } catch (...) {
        return ErrorResult(
            QStringLiteral("Failed to create session: unknown error"));
    }
}

}  // namespace interview::ui
