#include "services/real_llm_client.h"

#include <nlohmann/json.hpp>
#include <curl/curl.h>

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
}  // namespace

RealLLMClient::RealLLMClient(std::string api_url,
                             std::string api_key,
                             std::string model_name,
                             double temperature,
                             int max_tokens,
                             int timeout_seconds)
    : api_url_(std::move(api_url)),
      api_key_(std::move(api_key)),
      model_name_(std::move(model_name)),
      temperature_(temperature),
      max_tokens_(max_tokens),
      timeout_seconds_(timeout_seconds) {}


// 生成主问题列表。
    // 第三阶段可以先做一个最小版本：
    // 1. 先回退到固定题目
    // 2. 或者后面再接真实题目生成
std::vector<Question> RealLLMClient::GenerateQuestions(
    const std::string& resume_text,
    const std::string& job_description,
    int question_count) {
    (void)resume_text;
    (void)job_description;

    return BuildDefaultQuestions(question_count);
}

// 评估回答。
// 这是第三阶段最推荐优先接入真实能力的接口。
EvaluateResult RealLLMClient::EvaluateAnswer(
    const Question& question,
    const std::string& answer,
    const std::vector<AnswerRecord>& history) {
    std::string prompt = BuildEvaluatePrompt(question, answer, history);
    std::string response_text = CallModel(prompt);

    if (response_text.empty()) {
        return BuildFallbackEvaluateResult();
    }
    return ParseEvaluateResult(response_text);
}

    // 生成整场面试总结。
// 第三阶段初期可以先做一个保底实现，
// 等真实评分稳定后再切到真实总结生成。
std::string RealLLMClient::GenerateSummary(
    const std::vector<AnswerRecord>& records) {
    return BuildFallbackSummary(records);
}



// 构造“回答评估”用的 prompt。
// 作用：
//   把当前题目、当前回答、历史记录组织成模型输入。
std::string RealLLMClient::BuildEvaluatePrompt(
    const Question& question,
    const std::string& answer,
    const std::vector<AnswerRecord>& history) const {
    std::ostringstream oss;

    oss << "你是一个严格的技术面试官\n";
    oss << "请根据题目、候选人回答和历史上下文，对当前回答进行评分和反馈。\n";
    oss << "你必须返回 JSON，不能返回额外解释。\n\n";

    oss << "输出 JSON 格式如下：\n";
    oss << "{\n";
    oss << "  \"score\": 0,\n";
    oss << "  \"need_followup\": false,\n";
    oss << "  \"followup_question\": \"\",\n";
    oss << "  \"feedback\": \"\"\n";
    oss << "}\n\n";

    oss << "评分要求：\n";
    oss << "1. score 范围为 0 到 100。\n";
    oss << "2. need_followup 表示是否需要继续追问。\n";
    oss << "3. 如果 need_followup 为 true，则必须给出 followup_question。\n";
    oss << "4. feedback 要简洁、具体，指出回答的优点和不足。\n\n";

    oss << "当前题目：\n";
    oss << question.text << "\n\n";

    oss << "当前回答：\n";
    oss << answer << "\n\n";

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


// 解析模型返回结果。
// 第三阶段推荐让模型直接返回 JSON，
// 这里把 JSON 解析为 EvaluateResult。
// 如果解析失败，则直接返回 fallback。
EvaluateResult RealLLMClient::ParseEvaluateResult(const std::string& response_text) const {
    try {
        nlohmann::json json = nlohmann::json::parse(response_text);

        EvaluateResult result;
        result.score = json.value("score", 60);
        result.need_followup = json.value("need_followup", false);
        result.followup_question = json.value("followup_question", "");
        result.feedback = json.value("feedback", "模型已完成评估， 但反馈内容为空。");

        if (!result.need_followup) {
            result.followup_question.clear();
        }
        return result;
    } catch (...) {
        return BuildFallbackEvaluateResult();
    }
}

// 调用真实大模型接口。
// 这里按小马算力提供的 OpenAI-compatible Chat Completions 接口来发请求。
// 返回值：
//   成功时返回模型输出的 message.content
//   失败时返回空字符串，由上层走 fallback 逻辑
std::string RealLLMClient::CallModel(const std::string& prompt) const {
    // 基本配置校验。
    if (api_url_.empty() || api_key_.empty() || model_name_.empty()) {
        return "";
    }

    CURL* curl = curl_easy_init();
    if (curl == nullptr) {
        return "";
    }

    std::string response_body;

    nlohmann::json request_json;
    request_json["model"] = model_name_;
    request_json["temperature"] = temperature_;
    request_json["max_tokens"] = max_tokens_;
    request_json["stream"] = false;

    request_json["messages"] = nlohmann::json::array(
        {
            {
                {"role", "system"},
                {"content",
                 "你是一个严格的技术面试官。你必须严格按照要求返回 JSON。"}
            },
            {
                {"role", "user"},
                {"content", prompt}
            }
        });

    std::string request_body = request_json.dump();

    struct curl_slist* headers = nullptr;
    std::string auth_header = "Authorization: Bearer " + api_key_;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers, auth_header.c_str());

    curl_easy_setopt(curl, CURLOPT_URL, api_url_.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POST, 1L);

    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, request_body.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE,
                     static_cast<long>(request_body.size()));

    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_body);

    curl_easy_setopt(curl, CURLOPT_TIMEOUT,
                     static_cast<long>(timeout_seconds_));

    CURLcode code = curl_easy_perform(curl);

    long http_status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_status);

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (code != CURLE_OK) {
        LOG_WARN("llm http request failed at curl layer: code={}, msg={}",
                 static_cast<int>(code), curl_easy_strerror(code));
        return "";
    }

    if (http_status < 200 || http_status >= 300) {
        LOG_WARN("llm http non-2xx: status={}, body={}",
                 http_status, response_body);
        return "";
    }

    try {
        nlohmann::json response_json = nlohmann::json::parse(response_body);

        if (!response_json.contains("choices") ||
            !response_json["choices"].is_array() ||
            response_json["choices"].empty()) {
            return "";
        }

        const nlohmann::json& choice = response_json["choices"][0];

        if (!choice.contains("message") ||
            !choice["message"].contains("content")) {
            return "";
        }

        return choice["message"]["content"].get<std::string>();
    } catch (...) {
        return "";
    }
}

// 当真实模型调用失败时，提供兜底评估结果。
EvaluateResult RealLLMClient::BuildFallbackEvaluateResult() const {
    EvaluateResult result;
    result.score = 60;
    result.need_followup = false;
    result.followup_question.clear();
    result.feedback = "当前使用降级评估结果：模型调用失败，建议后续补充更完整的技术细节。";
    return result;
}

// 当真实总结失败时，提供兜底总结。
std::string RealLLMClient::BuildFallbackSummary(
    const std::vector<AnswerRecord>& records) const {
    std::ostringstream oss;

    int total_score = 0;
    for (const auto& record : records) {
        total_score += record.score;
    }

    int average_score = 0;
    if (!records.empty()) {
        average_score = total_score / static_cast<int>(records.size());
    }

    oss << "当前使用本地降级总结。";
    oss << "本次面试共记录 " << records.size() << " 条回答，";
    oss << "平均分约为 " << average_score << " 分。";

    if (average_score >= 85) {
        oss << "整体表现较好，回答较完整。";
    } else if (average_score >= 70) {
        oss << "基础概念基本清楚，但可以继续加强细节展开。";
    } else {
        oss << "回答仍有提升空间，建议加强基础概念和项目表达。";
    }

    return oss.str();
}

}  // namespace interview::services
