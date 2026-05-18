#include <cstring>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

#include "common/config.h"
#include "common/logger.h"
#include "interview/dialog_session.h"
#include "interview/interview_session.h"
#include "services/llm_client.h"
#include "services/mock_llm_client.h"
#include "services/pdf_parser.h"
#include "services/real_llm_client.h"
#include "services/real_realtime_client.h"

using namespace interview;

namespace {

void PrintUsage(const char* prog) {
    std::cerr
        << "Usage:\n"
        << "  " << prog << " [options] [resume.pdf]\n\n"
        << "Options:\n"
        << "  --mock-llm        Use MockLLMClient but keep real WSS + PortAudio\n"
        << "  -h, --help        Show this help\n\n"
        << "Notes:\n"
        << "  This Stage 8 demo requires config/local_config.json with real WSS keys.\n"
        << "  It captures mono 16 kHz int16 PCM and plays mono 24 kHz int16 PCM TTS.\n";
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

}  // namespace

int main(int argc, char* argv[]) {
    bool use_mock_llm = false;
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

        // The third constructor argument enables Stage 8 voice mode without
        // changing existing mock/text demos. DialogSession still owns all
        // lifecycle synchronization, including Stop() joining audio threads.
        session::DialogSession dialog(std::move(interview_session),
                                      std::move(realtime_client),
                                      true);
        dialog.Start();
        dialog.RunEventDriven();
        dialog.Stop();
        return 0;
    } catch (const std::exception& e) {
        LOG_ERROR("voice CLI failed: {}", e.what());
        return 2;
    }
}
