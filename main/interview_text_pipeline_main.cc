#include <chrono>
#include <condition_variable>
#include <cstring>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "common/config.h"
#include "common/logger.h"
#include "common/protocol.h"
#include "interview/dialog_session.h"
#include "interview/interview_session.h"
#include "services/llm_client.h"
#include "services/mock_llm_client.h"
#include "services/mock_realtime_client.h"
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

std::string ResumePreviewForLog(const std::string& s, std::size_t max_len) {
    std::string out;
    bool prev_space = false;
    for (char c : s) {
        if (out.size() >= max_len) {
            out += "...";
            break;
        }
        if (c == '\n' || c == '\r' || c == '\t') {
            if (!out.empty() && !prev_space) {
                out.push_back(' ');
                prev_space = true;
            }
            continue;
        }
        out.push_back(c);
        prev_space = (c == ' ');
    }
    while (!out.empty() && out.back() == ' ') {
        out.pop_back();
    }
    return out;
}

common::ParsedResponse MakeEvent(uint32_t event_id,
                                 std::string session_id = {},
                                 std::string payload_json = {}) {
    common::ParsedResponse e;
    e.event = event_id;
    e.session_id = std::move(session_id);
    e.payload_json = std::move(payload_json);
    return e;
}

void PrintUsage(const char* prog) {
    std::cerr
        << "Usage:\n"
        << "  " << prog << " [options] [resume.pdf]\n\n"
        << "Default flow:\n"
        << "  Load config/local_config.json, fallback to config/default_config.json\n"
        << "  -> parse PDF -> RealLLMClient/MockLLMClient -> MockRealtimeClient\n"
        << "  -> DialogSession::RunEventDriven()\n\n"
        << "Options:\n"
        << "  --mock-llm            Use MockLLMClient for offline state-machine test\n"
        << "  --stdin               Answer questions from stdin with mock realtime handshake\n"
        << "  --real-wss-smoke      Only test real WSS Connect -> ChatTextQuery -> TTS\n"
        << "  --text <text>         Text sent by --real-wss-smoke\n"
        << "  --wait-seconds <n>    TTS wait timeout for --real-wss-smoke\n"
        << "  -h, --help            Show this help\n\n"
        << "Examples:\n"
        << "  " << prog << "\n"
        << "  " << prog << " --mock-llm ./doc/resume.pdf\n"
        << "  " << prog << " --stdin --mock-llm\n"
        << "  " << prog << " --real-wss-smoke --text \"hello\" --wait-seconds 30\n";
}

constexpr int kPipelineQuestionCount = 3;

std::vector<common::ParsedResponse> BuildEventDrivenScript(
    const std::string& session_id) {
    std::vector<common::ParsedResponse> script;
    script.push_back(MakeEvent(common::events::kConnectionStarted));
    script.back().connect_id = "mock-connect-pipeline";
    script.push_back(MakeEvent(common::events::kSessionStarted, session_id));

    const char* asr_answers[] = {
        "I understand RAII and smart pointers.",
        "I know common STL containers and iterator invalidation cases.",
        "I have built a small epoll-based concurrent service.",
        "I would dispatch accepted connections to workers with queues.",
        "In muduo, Channel wraps fd events and EventLoop runs callbacks.",
        "I would unregister channels and stop loops during cleanup.",
    };
    static_assert(sizeof(asr_answers) / sizeof(asr_answers[0]) >=
                      kPipelineQuestionCount * 2,
                  "ASR script must cover main answers and followups");

    const int n = kPipelineQuestionCount * 2;
    for (int i = 0; i < n; ++i) {
        script.push_back(MakeEvent(common::events::kAsrInfo, session_id));
        script.push_back(
            MakeEvent(common::events::kAsrResult, session_id, asr_answers[i]));
        script.push_back(MakeEvent(common::events::kAsrEnded, session_id));
    }
    script.push_back(MakeEvent(common::events::kSessionFinished, session_id));
    return script;
}

std::vector<common::ParsedResponse> BuildStdinHandshakeScript(
    const std::string& session_id) {
    std::vector<common::ParsedResponse> script;
    script.push_back(MakeEvent(common::events::kConnectionStarted));
    script.back().connect_id = "mock-connect-stdin";
    script.push_back(MakeEvent(common::events::kSessionStarted, session_id));
    return script;
}

common::AppConfig LoadConfigWithFallback() {
    common::Config& config = common::Config::Instance();
    if (config.LoadFromFile("config/local_config.json")) {
        LOG_INFO("loaded config/local_config.json");
        return config.Snapshot();
    }
    if (config.LoadFromFile("config/default_config.json")) {
        LOG_WARN("config/local_config.json not found or invalid, using config/default_config.json");
        return config.Snapshot();
    }
    throw std::runtime_error(
        "failed to load config/local_config.json or config/default_config.json");
}

bool LoadLocalConfig(common::AppConfig& out_config) {
    common::Config& config = common::Config::Instance();
    if (!config.LoadFromFile("config/local_config.json")) {
        return false;
    }
    out_config = config.Snapshot();
    LOG_INFO("loaded config/local_config.json");
    return true;
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
        std::string reason;
        {
            std::lock_guard<std::mutex> lock(state.mutex);
            reason = state.failure_reason;
        }

        if (!reason.empty()) {
            LOG_ERROR("[smoke] session failed before id : {}", reason);
        } else {
            LOG_ERROR("[smoke] session id not received");
        }
        client.Close();
        return 4;
    }

    std::string session_id;
    {
        std::lock_guard<std::mutex> lock(state.mutex);
        session_id = state.session_id;
    }

    const nlohmann::json payload = {{"content", text}};
    if (!client.SendEvent(common::events::kChatTextQuery, session_id, payload)) {
        LOG_ERROR("[smoke] SendEvent(kChatTextQuery) failed");
        client.Close();
        return 5;
    }

    if (!WaitForTtsEnded(state, timeout)) {
        LOG_ERROR("[smoke] TTS did not finish within {} seconds", wait_seconds);
        std::string reason;
        {
            std::lock_guard<std::mutex> lock(state.mutex);
            reason = state.failure_reason;
        }
        if (!reason.empty()) {
            LOG_ERROR("[smoke] session failed before id : {}", reason);
        } else {
            LOG_ERROR("[smoke] session id not received");
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
    bool use_stdin = false;
    bool run_real_wss_smoke = false;
    std::string smoke_text = "Hello, please briefly introduce this interview.";
    int wait_seconds = 20;
    std::string pdf_path;

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
        if (std::strcmp(arg, "--stdin") == 0) {
            use_stdin = true;
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
        if (pdf_path.empty()) {
            pdf_path = arg;
        } else {
            std::cerr << "extra argument: " << arg << "\n";
            PrintUsage(argv[0]);
            return 1;
        }
    }

    if (!common::Logger::Init()) {
        return 1;
    }

    if (run_real_wss_smoke) {
        common::AppConfig config;
        if (!LoadLocalConfig(config)) {
            LOG_ERROR("real WSS smoke requires config/local_config.json with real keys");
            return 2;
        }
        return RunRealWssSmoke(config, smoke_text, wait_seconds);
    }

    if (pdf_path.empty()) {
        pdf_path = "doc/resume.pdf";
    }

    common::AppConfig config;
    try {
        config = LoadConfigWithFallback();
    } catch (const std::exception& e) {
        LOG_ERROR("load config failed: {}", e.what());
        return 1;
    }

    services::PDFParser parser;
    std::string resume_text;
    if (!pdf_path.empty() && parser.IsValidPDF(pdf_path)) {
        resume_text = parser.ExtractText(pdf_path);
        LOG_INFO("PDF parsed path=\"{}\" chars={}", pdf_path, resume_text.size());
        if (!resume_text.empty()) {
            LOG_INFO("resume preview: {}", ResumePreviewForLog(resume_text, 260));
        } else {
            LOG_WARN("PDF path is valid but no text was extracted");
        }
    } else {
        LOG_WARN("PDF not parsed, invalid path or non-PDF: {}", pdf_path);
    }

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

    const std::string k_sid =
        use_stdin ? "stdin-mock-session" : "mock-session-pipeline";

    std::vector<common::ParsedResponse> script =
        use_stdin ? BuildStdinHandshakeScript(k_sid)
                  : BuildEventDrivenScript(k_sid);

    const auto interval = use_stdin ? std::chrono::milliseconds(80)
                                    : std::chrono::milliseconds(200);

    auto rt =
        std::make_unique<services::MockRealtimeClient>(std::move(script),
                                                       interval);

    session::DialogSession dialog(std::move(interview_session), std::move(rt));
    dialog.Start();

    if (use_stdin) {
        LOG_INFO("stdin mode: answer questions in the console");
        dialog.Run();
    } else {
        LOG_INFO("event-driven mode: MockRealtimeClient dispatches ASR script");
        dialog.RunEventDriven();
    }

    dialog.Stop();
    return 0;
}
