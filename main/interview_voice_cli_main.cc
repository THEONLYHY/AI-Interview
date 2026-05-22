#include <chrono>
#include <condition_variable>
#include <cstring>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>

#include <nlohmann/json.hpp>

#include "common/config.h"
#include "common/logger.h"
#include "common/protocol.h"
#include "interview/dialog_session.h"
#include "interview/interview_session.h"
#include "services/llm_client.h"
#include "services/mock_llm_client.h"
#include "services/pdf_parser.h"
#include "services/real_llm_client.h"
#include "services/real_realtime_client.h"

using namespace interview;

namespace {

struct RealWssSmokeState {
    std::mutex mutex;
    std::condition_variable cv;
    std::string session_id;
    std::string failure_reason;
    std::size_t tts_bytes = 0;
    bool tts_ended = false;
    bool failed = false;
};

void PrintUsage(const char* prog) {
    std::cerr
        << "Usage:\n"
        << "  " << prog << " [options] [resume.pdf]\n\n"
        << "Options:\n"
        << "  --mock-llm            Use MockLLMClient but keep real WSS + PortAudio\n"
        << "  --real-wss-smoke      Only test real WSS Connect -> ChatTextQuery -> TTS\n"
        << "  --text <text>         Text sent by --real-wss-smoke\n"
        << "  --wait-seconds <n>    TTS wait timeout for --real-wss-smoke\n"
        << "  -h, --help            Show this help\n\n"
        << "Notes:\n"
        << "  The voice session requires config/local_config.json with real keys.\n"
        << "  --mock-llm replaces only the LLM client; WSS and audio stay real.\n"
        << "  The CLI captures mono 16 kHz int16 PCM and plays mono 24 kHz int16 PCM TTS.\n\n"
        << "Examples:\n"
        << "  " << prog << " --mock-llm ./doc/resume.pdf\n"
        << "  " << prog << " --real-wss-smoke --text \"hello\" --wait-seconds 30\n";
}

common::AppConfig LoadLocalConfigOrThrow() {
    common::Config& config = common::Config::Instance();
    if (!config.LoadFromFile("config/local_config.json")) {
        throw std::runtime_error(
            "config/local_config.json is required for the real voice CLI");
    }
    LOG_INFO("loaded config/local_config.json");
    return config.Snapshot();
}

std::string LoadResumeText(const std::string& pdf_path) {
    if (pdf_path.empty()) {
        return {};
    }

    services::PDFParser parser;
    if (!parser.IsValidPDF(pdf_path)) {
        LOG_WARN("resume PDF not found or invalid: {}", pdf_path);
        return {};
    }

    std::string text = parser.ExtractText(pdf_path);
    LOG_INFO("resume PDF parsed: path='{}', chars={}", pdf_path, text.size());
    return text;
}

bool WaitForSessionId(RealWssSmokeState& state, std::chrono::seconds timeout) {
    std::unique_lock<std::mutex> lock(state.mutex);
    return state.cv.wait_for(lock, timeout, [&] {
               return !state.session_id.empty() || state.failed;
           }) &&
           !state.session_id.empty() && !state.failed;
}

bool WaitForTtsEnded(RealWssSmokeState& state, std::chrono::seconds timeout) {
    std::unique_lock<std::mutex> lock(state.mutex);
    return state.cv.wait_for(lock, timeout, [&] {
               return state.tts_ended || state.failed;
           }) &&
           state.tts_ended && !state.failed;
}

int RunRealWssSmoke(const common::AppConfig& config,
                    const std::string& text,
                    int wait_seconds) {
    RealWssSmokeState state;
    services::RealRealtimeClient client(config.ws.base_url);

    LOG_INFO("[smoke] text smoke input_mod={} wait_seconds={} text_bytes={}",
             config.dialog.input_mod, wait_seconds, text.size());
    if (config.dialog.input_mod == "audio") {
        LOG_WARN("[smoke] dialog.input_mod=audio; text smoke sends no PCM and "
                 "may hit DialogAudioIdleTimeoutError");
    }

    client.SetEventHandler([&state](const common::ParsedResponse& evt) {
        if (evt.code != 0) {
            LOG_ERROR("[smoke] server error code={} payload={}",
                      evt.code, evt.payload_json);
            {
                std::lock_guard<std::mutex> lock(state.mutex);
                state.failed = true;
                state.failure_reason =
                    "server error code=" + std::to_string(evt.code) +
                    " payload=" + evt.payload_json;
            }
            state.cv.notify_all();
            return;
        }

        switch (evt.event) {
        case common::events::kConnectionStarted:
            LOG_INFO("[smoke] kConnectionStarted connect_id={}", evt.connect_id);
            break;

        case common::events::kSessionStarted:
            LOG_INFO("[smoke] kSessionStarted session_id={}", evt.session_id);
            {
                std::lock_guard<std::mutex> lock(state.mutex);
                state.session_id = evt.session_id;
            }
            state.cv.notify_all();
            break;

        case common::events::kTtsResponse:
            if (evt.is_binary) {
                std::lock_guard<std::mutex> lock(state.mutex);
                state.tts_bytes += evt.payload_bytes.size();
            }
            LOG_INFO("[smoke] kTtsResponse bytes={}", evt.payload_bytes.size());
            break;

        case common::events::kTtsEnded:
            LOG_INFO("[smoke] kTtsEnded");
            {
                std::lock_guard<std::mutex> lock(state.mutex);
                state.tts_ended = true;
            }
            state.cv.notify_all();
            break;

        case common::events::kSessionFailed:
        case common::events::kConnectionFailed:
            LOG_ERROR("[smoke] failure event={} payload={}",
                      evt.event, evt.payload_json);
            {
                std::lock_guard<std::mutex> lock(state.mutex);
                state.failed = true;
                state.failure_reason = evt.payload_json;
            }
            state.cv.notify_all();
            break;

        default:
            LOG_DEBUG("[smoke] event={} json={} binary_bytes={}",
                      evt.event, evt.payload_json, evt.payload_bytes.size());
            break;
        }
    });

    if (!client.Connect()) {
        LOG_ERROR("[smoke] Connect() failed");
        return 3;
    }

    const auto timeout = std::chrono::seconds(wait_seconds);
    if (!WaitForSessionId(state, timeout)) {
        LOG_ERROR("[smoke] session id not received");
        client.Close();
        return 4;
    }

    std::string session_id;
    {
        std::lock_guard<std::mutex> lock(state.mutex);
        session_id = state.session_id;
    }

    const nlohmann::json payload =
        common::Protocol::BuildReadAloudTextQueryPayload(text);
    if (!client.SendEvent(common::events::kChatTextQuery, session_id, payload)) {
        LOG_ERROR("[smoke] SendEvent(kChatTextQuery) failed");
        client.Close();
        return 5;
    }

    if (!WaitForTtsEnded(state, timeout)) {
        std::string reason;
        {
            std::lock_guard<std::mutex> lock(state.mutex);
            reason = state.failure_reason;
        }
        if (!reason.empty()) {
            LOG_ERROR("[smoke] session failed: {}", reason);
        } else {
            LOG_ERROR("[smoke] TTS did not finish within {} seconds",
                      wait_seconds);
        }
        client.Close();
        return 6;
    }

    {
        std::lock_guard<std::mutex> lock(state.mutex);
        LOG_INFO("[smoke] success, received TTS bytes={}", state.tts_bytes);
    }
    client.Close();
    return 0;
}

}  // namespace

int main(int argc, char* argv[]) {
    bool use_mock_llm = false;
    bool run_real_wss_smoke = false;
    std::string smoke_text = "Hello, please briefly introduce this interview.";
    int wait_seconds = 20;
    std::string pdf_path = "doc/resume.pdf";

    for (int i = 1; i < argc; ++i) {
        const char* arg = argv[i];
        if (std::strcmp(arg, "-h") == 0 || std::strcmp(arg, "--help") == 0) {
            PrintUsage(argv[0]);
            return 0;
        }
        if (std::strcmp(arg, "--mock-llm") == 0) {
            use_mock_llm = true;
            continue;
        }
        if (std::strcmp(arg, "--real-wss-smoke") == 0) {
            run_real_wss_smoke = true;
            continue;
        }
        if (std::strcmp(arg, "--text") == 0 && i + 1 < argc) {
            smoke_text = argv[++i];
            continue;
        }
        if (std::strcmp(arg, "--wait-seconds") == 0 && i + 1 < argc) {
            wait_seconds = std::stoi(argv[++i]);
            if (wait_seconds <= 0) {
                wait_seconds = 20;
            }
            continue;
        }
        if (arg[0] == '-') {
            std::cerr << "unknown option: " << arg << "\n";
            PrintUsage(argv[0]);
            return 1;
        }
        pdf_path = arg;
    }

    if (!common::Logger::Init()) {
        return 1;
    }

    try {
        const common::AppConfig config = LoadLocalConfigOrThrow();
        if (run_real_wss_smoke) {
            return RunRealWssSmoke(config, smoke_text, wait_seconds);
        }

        std::string resume_text = LoadResumeText(pdf_path);

        std::unique_ptr<services::LLMClient> llm;
        if (use_mock_llm) {
            LOG_INFO("LLM mode: MockLLMClient");
            llm = std::make_unique<services::MockLLMClient>();
        } else {
            LOG_INFO("LLM mode: RealLLMClient ({})", config.llm.model);
            llm = std::make_unique<services::RealLLMClient>(
                config.llm.api_url, config.llm.api_key, config.llm.model,
                config.llm.temperature, config.llm.max_tokens,
                config.llm.timeout_seconds);
        }

        auto interview_session = std::make_unique<session::InterviewSession>(
            std::move(llm), std::move(resume_text));
        auto realtime_client =
            std::make_unique<services::RealRealtimeClient>(config.ws.base_url);

        session::DialogSession dialog(std::move(interview_session),
                                      std::move(realtime_client),
                                      true);
        dialog.Start();
        if (dialog.State() == common::DialogState::kStopped) {
            return 1;
        }
        dialog.RunEventDriven();
        dialog.Stop();
        return 0;
    } catch (const std::exception& e) {
        LOG_ERROR("voice CLI failed: {}", e.what());
        return 2;
    }
}
