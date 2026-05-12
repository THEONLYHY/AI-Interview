#include "services/real_llm_client.h"

#include <nlohmann/json.hpp>
#include <curl/curl.h>

#include <cstddef>
#include <sstream>
#include <string>
#include <vector>
#include <utility>

#include "common/logger.h"

namespace interview::services {

using interview::common::AnswerRecord;
using interview::common::EvaluateResult;
using interview::common::Question;

namespace {

size_t WriteCallback(void* contents, size_t size, size_t nmemb, void* userp) {
    size_t total_size = size * nmemb;
    std::string* response = static_cast<std::string*>(userp);
    response->append(static_cast<char*>(contents), total_size);
    return total_size;
}
    // 第三阶段先给一个固定题目兜底，避免题目生成还没接真实模型时影响主流程。
std::vector<Question> BuildDefaultQuestions(int question_count) {
    std::vector<Question> questions = {
        {1, "请介绍一下 RAII", false, -1},
        {2, "请说一下智能指针的作用", false, -1},
        {3, "请解释 epoll 和 select 的区别", false, -1}
    };

    if (question_count <= 0 ||
        question_count >= static_cast<int>(questions.size())) {
        return questions;
    }

    return std::vector<Question>(questions.begin(),
                                 questions.begin() + question_count);
}

// 从 Chat Completions 响应 JSON 里取出 assistant 的纯文本 content。
// 失败返回空字符串（上层走 fallback）。
std::string ParseChatCompletionContent(const std::string& response_body) {
    try {
        const nlohmann::json response_json = nlohmann::json::parse(response_body);
        if (!response_json.contains("choices") ||
            !response_json["choices"].is_array() ||
            response_json["choices"].empty()) {
            return {};
        }
        const auto& choice = response_json["choices"][0];
        if (!choice.contains("message") || !choice["message"].contains("content")) {
            return {};
        }
        return choice["message"]["content"].get<std::string>();
    } catch (...) {
        return {};
    }
}

// 模型偶尔会输出带 Markdown 代码块的 JSON；尝试截取首尾花括号再解析。
std::string StripJsonCandidate(std::string s) {
    const auto first = s.find('{');
    const auto last = s.find('}');
    if (first != std::string::npos && last != std::string::npos && last > first) {
        return s.substr(first, last - first + 1);
    }
    return s;
}

// 评估接口要求模型只输出 JSON；这里做一次宽松提取。
EvaluateResult ParseEveluateJsonLoose(const std::string& text) {
    try {
        const nlohmann::json j = 
            nlohmann::json::parse(StripJsonCandidate(text));
        EvaluateResult r;
        r.score = j.value("score", 60);
        r.need_followup = j.value("need_followup", false);
        r.followup_question = j.value("followup_question", "");
        r.feedback = j.value("feedback", "模型已完成评估，但反馈内容为空");
        if (!r.need_followup) {
            r.followup_question.clear();
        }
        return r;
    } catch (...) {
        return {};
    }
}

}  // namespace

// ======================== Impl：集中放 curl + 业务解析 ========================    
class RealLLMClient::Impl {
public:
    Impl(std::string api_url, std::string api_key, std::string model_name,
       double temperature, int max_tokens, int timeout_seconds)
      : api_url_(std::move(api_url)),
        api_key_(std::move(api_key)),
        model_name_(std::move(model_name)),
        temperature_(temperature),
        max_tokens_(max_tokens),
        timeout_seconds_(timeout_seconds) {}

    // 统一 HTTP：POST JSON，返回响应体；失败返回空串（内部已打日志）。
    std::string HttpPostJson(const std::string& url,
                            const std::vector<std::pair<std::string, std::string>>& headers,
                            const std::string& body) const {
        if (url.empty()) {
            LOG_WARN("HttpPostJson: empty url");
            return {};
        }

        // “一次HTTP请求的上下文对象”
        CURL* curl = curl_easy_init();
        if (curl == nullptr) {
            LOG_WARN("HttpPostJson: curl_easy_init failed");
            return {};
        }
        // 把headers 参数转换成 libcurl 认识的链表格式
        // libcurl 要求的是curl_slist*
        curl_slist* header_list = nullptr;
        for (const auto& h : headers) {
            // 比如：
            // ("Content-Type", "application/json")
            // 会被拼成：
            // "Content-Type: application/json"
            const std::string line = h.first + ": " + h.second;
            header_list = curl_slist_append(header_list, line.c_str());
        }
        // 准备一个字符串用来接收服务端返回的响应体
        // 后面WriteCallback 会不断把收到的数据追加到这里
        std::string response_body;
        // 设置请求的目标url
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        // 设置HTTP请求头
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, header_list);
        // 明确指定这是一个POST 请求
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        // 设置POST 请求体数据
        // 这里body 通常就是一个 JSON 字符串
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
        // 显式告诉libcurl, 请求体长度是多少
        // 这样即使 body 里将来包含 '\0'，libcurl 也能正确处理。
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE,
                        static_cast<long>(body.size()));
        // 设置写回调函数。
        // 当服务器返回数据时，libcurl 会调用 WriteCallback。
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
        // 把 response_body 的地址传给回调函数。
        // 这样回调就知道把收到的数据写到哪里。
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_body);
        // 设置超时时间，防止请求无限卡住。
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, static_cast<long>(timeout_seconds_));

        // 真正执行这次 HTTP 请求。
        const CURLcode code = curl_easy_perform(curl);
        // 获取 HTTP 状态码，比如 200、401、500。
        long http_status = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_status);
        // 释放请求头链表。
        curl_slist_free_all(header_list);
        // 释放 curl handle。
        curl_easy_cleanup(curl);

        // 先检查网络层 / libcurl 层是否执行成功。
        // 注意：这和 HTTP 200/500 不是一回事。
        // code != CURLE_OK 说明请求可能压根没成功发出去，
        // 或者连接、DNS、超时等环节出错。
        if (code != CURLE_OK) {
            LOG_WARN("HttpPostJson: curl error {}: {}", static_cast<int>(code),
            curl_easy_strerror(code));
            return {};
        }
        // 再检查 HTTP 状态码是不是 2xx。
        // 如果不是 2xx，说明服务端返回了错误语义。
        if (http_status < 200 || http_status >= 300) {
            LOG_WARN("HttpPostJson: HTTP {} body: {}", http_status, response_body);
            return {};
        }
        // 一切正常，返回服务端响应体。
        return response_body;
    }
    
    // 构建OpenAI 兼容 Chat Completions 请求，拿到 assistant 的content文本.
    std::string CallModel(const std::string& user_prompt,
                            const std::string& system_prompt) const {
        if (api_url_.empty() || api_key_.empty() || model_name_.empty()) {
            LOG_WARN("CallModel: missing api_url / api_key / model_name");
            return {};
        }

        nlohmann::json request_json;
        request_json["model"] = model_name_;
        request_json["temperature"] = temperature_;
        request_json["max_tokens"] = max_tokens_;
        request_json["stream"] = false;
        request_json["messages"] = nlohmann::json::array({
            nlohmann::json{{"role", "system"}, {"content", system_prompt}},
            nlohmann::json{{"role", "user"}, {"content", user_prompt}},
        });
        const std::string req_body = request_json.dump();
        std::vector<std::pair<std::string, std::string>> hdrs;
        hdrs.emplace_back("Content-Type", "application/json");
        hdrs.emplace_back("Authorization", "Bearer " + api_key_);

        const std::string resp = HttpPostJson(api_url_, hdrs, req_body);
        if (resp.empty()) {
            return {};
        }
        return ParseChatCompletionContent(resp);
    }
    
    std::string BuildQuestionPrompt(const std::string& resume_text,
                                    const std::string& job_description,
                                    int question_count) const {
        std::ostringstream oss;
        oss << "你是技术面试官。请根据候选人简历与目标岗位，生成恰好 "
        << question_count << " 道**主问题**（不要追问子题）。\n\n";
        oss << "你必须只输出一个 JSON 数组，不要 Markdown，不要多余说明。\n";
        oss << "每一项格式：{\"id\":整型,\"text\":\"题目字符串\"}，id 从 1 递增。\n\n";
        oss << "岗位描述：\n" << job_description << "\n\n";
        oss << "简历文本（可能为空）：\n" << resume_text << "\n";
        return oss.str();
    }

    std::vector<Question> ParseQuestionArray(
        const std::string& response_text) const {
        try {
            const nlohmann::json root = nlohmann::json::parse(StripJsonCandidate(response_text));
            if (!root.is_array()) {
                return {};
            }
            std::vector<Question> out;
            out.reserve(root.size());
            for (const auto& item : root) {
                Question q;
                q.id = item.value("id", static_cast<int>(out.size()) + 1);
                q.text = item.value("text", std::string{});
                q.is_followup = false;
                q.parent_question_id = -1;
                if (!q.text.empty()) {
                    out.push_back(std::move(q));
                }
            }
            return out;
        } catch (...) {
            return {};
        }
    }
    
    std::vector<Question> GenerateQuestion(const std::string& resume_text,
                                            const std::string& job_description,
                                            int question_count) {
        if (question_count <= 0) {
            return {};
        }
        const std::string prompt = 
            BuildQuestionPrompt(resume_text, job_description, question_count);
        const std::string system = 
            "你必须严格按要求只输出 JSON 数组；不要输出任何其他文字。";
        const std::string content = CallModel(prompt, system);
        if (content.empty()) {
            LOG_WARN("GenerateQuestions: empty model content, use default questions");
            return BuildDefaultQuestions(question_count);
        }
        auto parsed = ParseQuestionArray(content);
        if (static_cast<int>(parsed.size()) < question_count) {
            LOG_WARN(
                "GenerateQuestions: parse got {} questions, expect {}, fallback",
                parsed.size(), question_count);
            return BuildDefaultQuestions(question_count);
        }

        if (static_cast<int>(parsed.size()) > question_count) {
            parsed.resize(static_cast<std::size_t>(question_count));
        }
        return parsed;
    }

    std::string BuildEvaluatePrompt(const Question& question,
                                    const std::string& answer,
                                    const std::vector<AnswerRecord>& history) const {
        std::ostringstream oss;
        oss << "你是一个严格的技术面试官。\n";
        oss << "请根据题目、候选人回答和历史上下文，对当前回答进行评分和反馈。\n";
        oss << "你必须只返回 JSON，不要 Markdown，不要额外解释。\n\n";
        oss << "JSON 格式：\n";
        oss << "{\n";
        oss << "  \"score\": 0,\n";
        oss << "  \"need_followup\": false,\n";
        oss << "  \"followup_question\": \"\",\n";
        oss << "  \"feedback\": \"\"\n";
        oss << "}\n\n";
        oss << "当前题目：\n" << question.text << "\n\n";
        oss << "当前回答：\n" << answer << "\n\n";
        oss << "历史上下文：\n";
        if (history.empty()) {
            oss << "无\n";
        } else {
            for (std::size_t i = 0; i < history.size(); ++i) {
                oss << "记录 " << (i + 1) << ":\n";
                oss << "题目: " << history[i].question_text << "\n";
                oss << "回答: " << history[i].answer_text << "\n";
                oss << "得分: " << history[i].score << "\n";
                oss << "是否为追问回答: "
                    << (history[i].is_followup_answer ? "true" : "false") << "\n\n";
            }
        }
        return oss.str();
    }
    EvaluateResult BuildFallbackEvaluateResult() const {
        EvaluateResult r;
        r.score = 60;
        r.need_followup = false;
        r.followup_question.clear();
        r.feedback = "降级评估：模型调用失败或解析失败。";
        return r;
    }

    EvaluateResult EvaluateAnswer(const Question& question,
                                    const std::string& answer,
                                    const std::vector<AnswerRecord>& history) {
        const std::string prompt = BuildEvaluatePrompt(question, answer, history);
        const std::string system = 
                "你是技术面试官。你必须只输出合法 JSON，键名与示例完全一致。";
        const std::string content = CallModel(prompt, system);
        if (content.empty()) {
            return BuildFallbackEvaluateResult();
        }
        EvaluateResult r = ParseEveluateJsonLoose(content);
        if (r.feedback.empty() && r.score == 0 && !r.need_followup) {
            return BuildFallbackEvaluateResult();
        }
        return r;
    }

    std::string BuildSummaryPrompt(
            const std::vector<AnswerRecord>& records) const {
        std::ostringstream oss;
        oss << "下面是整场模拟面试的问答记录，请用中文写一段总结（纯文本，不要 JSON）。\n";
        oss << "包含：整体表现、主要优缺点、是否达到中级 C++ 工程师预期。\n\n";
        for (std::size_t i = 0; i < records.size(); ++i) {
            const auto& rec = records[i];
            oss << "--- 记录 " << (i + 1) << " ---\n";
            oss << "题目: " << rec.question_text << "\n";
            oss << "回答: " << rec.answer_text << "\n";
            oss << "得分: " << rec.score << "\n\n";
        }
        return oss.str();
    }

    std::string BuildFallbackSummary(
        const std::vector<AnswerRecord>& records) const {
        std::ostringstream oss;
        int total = 0;
        for (const auto& rec : records) {
            total += rec.score;
        }
        const int avg =
            records.empty() ? 0 : total / static_cast<int>(records.size());
        oss << "本地降级总结：共 " << records.size() << " 条回答，平均分约 " << avg
            << "。";
        return oss.str();
    }

    std::string GenerateSummary(const std::vector<AnswerRecord>& records) {
        const std::string prompt = BuildSummaryPrompt(records);
        const std::string system = "你是面试官助理，只输出简短中文总结段落。";
        std::string content = CallModel(prompt, system);
        if (content.empty()) {
            return BuildFallbackSummary(records);
        }

        // 去掉首位空白，避免无意义空串被当成成功
        const auto first = content.find_first_not_of(" \t\n\r");
        const auto last = content.find_last_not_of(" \t\n\r");
        if (first == std::string::npos) {
            return BuildFallbackSummary(records);
        }
        content = content.substr(first, last - first + 1);
        return content;
    }
private:
    std::string api_url_;
    std::string api_key_;
    std::string model_name_;
    double temperature_;
    int max_tokens_;
    int timeout_seconds_;
};

// ======================== 对外壳：仅转发 ========================
RealLLMClient::RealLLMClient(std::string api_url,
                             std::string api_key,
                             std::string model_name,
                             double temperature,
                             int max_tokens,
                             int timeout_seconds)
    : impl_(std::make_unique<Impl>(std::move(api_url),
                                    std::move(api_key),
                                    std::move(model_name),
                                    temperature,
                                    max_tokens,
                                    timeout_seconds)) {}
RealLLMClient::~RealLLMClient() = default;

// 生成主问题列表。
    // 第三阶段可以先做一个最小版本：
    // 1. 先回退到固定题目
    // 2. 或者后面再接真实题目生成
std::vector<Question> RealLLMClient::GenerateQuestions(
    const std::string& resume_text,
    const std::string& job_description,
    int question_count) {
    return impl_->GenerateQuestion(resume_text, job_description, question_count);
}

// 评估回答。
// 这是第三阶段最推荐优先接入真实能力的接口。
EvaluateResult RealLLMClient::EvaluateAnswer(
    const Question& question,
    const std::string& answer,
    const std::vector<AnswerRecord>& history) {
    return impl_->EvaluateAnswer(question, answer, history);
}

    // 生成整场面试总结。
// 第三阶段初期可以先做一个保底实现，
// 等真实评分稳定后再切到真实总结生成。
std::string RealLLMClient::GenerateSummary(
    const std::vector<AnswerRecord>& records) {
    return impl_->GenerateSummary(records);
}

}  // namespace interview::services