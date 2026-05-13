#include <chrono>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

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

using namespace interview;

namespace {

// 将简历正文压成单行并截断，便于日志确认 PDF 抽取是否生效（不含换行与过长输出）。
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

common::ParsedResponse MakeEvent(uint32_t event_id, std::string session_id = {},
        std::string payload_json = {}) {
    common::ParsedResponse e;
    e.event = event_id;
    e.session_id = std::move(session_id);
    e.payload_json = std::move(payload_json);
    return e;
}

void PrintUsage(const char* prog) {
    std::cerr << "用法:\n"
              << "  " << prog << " [选项] [简历.pdf]\n\n"
              << "默认流程（与仓库主线一致）：\n"
              << "  读取 config/default_config.json → 抽取 PDF 为纯文本（日志会打出预览）\n"
              << "  → RealLLMClient / MockLLMClient 出题 → MockRealtimeClient 脚本模拟 ASR\n"
              << "  → DialogSession::RunEventDriven()\n\n"
              << "选项:\n"
              << "  --mock-llm     使用 MockLLMClient（无需 API Key，离线跑状态机）\n"
              << "  --stdin        控制台逐题输入回答（仍为 Mock 实时握手，不发 ASR 脚本）\n"
              << "  -h, --help     显示本说明\n\n"
              << "示例:\n"
              << "  " << prog << "\n"
              << "  " << prog << " --mock-llm ./doc/resume.pdf\n"
              << "  " << prog << " --stdin --mock-llm\n";
}

// 与 InterviewSession::Start 中出题数量一致；每题 EvaluateAnswer 可能产生一轮追问，
// HandleAsrFinalized 每轮追问也消耗一整段 ASR，故脚本轮数至少为 question_count * 2。
constexpr int kPipelineQuestionCount = 3;

std::vector<common::ParsedResponse> BuildEventDrivenScript(const std::string& session_id) {
    std::vector<common::ParsedResponse> script;
    script.push_back(MakeEvent(common::events::kConnectionStarted));
    script.back().connect_id = "mock-connect-pipeline";
    script.push_back(MakeEvent(common::events::kSessionStarted, session_id));

    const char* asr_answers[] = {
            // Q1 主答 / 追问
            "我熟悉智能指针与 RAII，能说明资源与生命周期管理。",
            "我了解常用 STL 容器及迭代器失效场景。",
            // Q2 主答 / 追问
            "我用 epoll 写过简单高并发服务骨架。",
            "主从 Reactor 里用队列把连接派给工作线程，并用互斥或无锁队列做任务分发。",
            // Q3 主答 / 追问（追问不一定会发生，多出的脚本轮次不会被消费）
            "muduo 里用 Channel 绑 fd，Poller 返活跃通道后在 EventLoop 线程里调回调。",
            "析构里注销 channel、停掉 loop，并用 RAII 封装 MutexLockGuard。",
    };
    static_assert(sizeof(asr_answers) / sizeof(asr_answers[0]) >= kPipelineQuestionCount * 2,
            "ASR 脚本长度需覆盖每题主答+追问的上界");

    const int n = kPipelineQuestionCount * 2;
    for (int i = 0; i < n; ++i) {
        const char* t = asr_answers[i];
        script.push_back(MakeEvent(common::events::kAsrInfo, session_id));
        script.push_back(MakeEvent(common::events::kAsrResult, session_id, t));
        script.push_back(MakeEvent(common::events::kAsrEnded, session_id));
    }
    script.push_back(MakeEvent(common::events::kSessionFinished, session_id));
    return script;
}

std::vector<common::ParsedResponse> BuildStdinHandshakeScript(const std::string& session_id) {
    std::vector<common::ParsedResponse> script;
    script.push_back(MakeEvent(common::events::kConnectionStarted));
    script.back().connect_id = "mock-connect-stdin";
    script.push_back(MakeEvent(common::events::kSessionStarted, session_id));
    return script;
}

}  // namespace

int main(int argc, char* argv[]) {
    bool use_mock_llm = false;
    bool use_stdin = false;
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
        if (arg[0] == '-') {
            std::cerr << "未知选项: " << arg << "\n";
            PrintUsage(argv[0]);
            return 1;
        }
        if (pdf_path.empty()) {
            pdf_path = arg;
        } else {
            std::cerr << "多余的参数: " << arg << "\n";
            PrintUsage(argv[0]);
            return 1;
        }
    }

    if (pdf_path.empty()) {
        pdf_path = "doc/resume.pdf";
    }

    if (!common::Logger::Init()) {
        return 1;
    }

    common::AppConfig config;
    try {
        config = common::LoadConfig("config/default_config.json");
    } catch (const std::exception& e) {
        LOG_ERROR("load config failed : {}", e.what());
        return 1;
    }

    services::PDFParser parser;
    std::string resume_text;
    if (!pdf_path.empty() && parser.IsValidPDF(pdf_path)) {
        resume_text = parser.ExtractText(pdf_path);
        LOG_INFO("PDF 解析完成 path=\"{}\" chars={}", pdf_path, resume_text.size());
        if (!resume_text.empty()) {
            LOG_INFO("简历预览（截断）: {}", ResumePreviewForLog(resume_text, 260));
        } else {
            LOG_WARN("PDF 路径有效但未解析出文本，后续出题可能无简历上下文");
        }
    } else {
        LOG_WARN("未解析 PDF（无效路径或非 PDF）: {}", pdf_path);
    }

    std::unique_ptr<services::LLMClient> llm;
    if (use_mock_llm) {
        LOG_INFO("LLM mode: MockLLMClient（固定题库；题干会标注已载入简历规模）");
        llm = std::make_unique<services::MockLLMClient>();
    } else {
        LOG_INFO("LLM mode: RealLLMClient ({})，出题将携带简历全文", config.llm.model);
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

    const auto interval =
            use_stdin ? std::chrono::milliseconds(80)
                      : std::chrono::milliseconds(200);

    auto rt = std::make_unique<services::MockRealtimeClient>(std::move(script),
                                                             interval);

    session::DialogSession dialog(std::move(interview_session), std::move(rt));
    dialog.Start();

    if (use_stdin) {
        LOG_INFO("交互模式：请在控制台逐题作答（题目会通过 SpeakText → Mock 日志侧可见）");
        dialog.Run();
    } else {
        LOG_INFO("事件驱动模式：MockRealtimeClient 自动投递 ASR 脚本");
        dialog.RunEventDriven();
    }

    dialog.Stop();
    return 0;
}
