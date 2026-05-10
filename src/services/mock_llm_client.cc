#include "services/mock_llm_client.h"

#include <sstream>

namespace interview::services {

using interview::common::AnswerRecord;
using interview::common::EvaluateResult;
using interview::common::Question;

// 生成固定主问题列表。
// 第三阶段接口已经支持 resume_text、job_description 和 question_count，
// 但 mock 实现里先不使用前两个参数，只根据 question_count 截取固定题目。
std::vector<Question> MockLLMClient::GenerateQuestions(
    const std::string& resume_text,
    const std::string& job_description,
    int question_count) {

    (void)resume_text;
    (void)job_description;

    std::vector<Question> questions = {
        {1, "请介绍一下 RAII", false, -1},
        {2, "请说一下智能指针的作用", false, -1},
        {3, "请解释 epoll 和 select 的区别", false, -1}
    };

    if (question_count <= 0 ||
        question_count >= static_cast<int>(questions.size())) {
        return questions;
    }
    return std::vector<Question>(questions.begin(), questions.begin() + question_count);
}

// 对用户回答做简单评估。
// 第三阶段接口加入了 history 参数，用于真实 LLM 上下文；
// mock 版本里暂时忽略 history，继续按回答长度做简单评分。
EvaluateResult MockLLMClient::EvaluateAnswer(
    const Question& question,
    const std::string& answer,
    const std::vector<AnswerRecord>& history) {
    (void)history;
    EvaluateResult result;

    if (answer.size() < 10) {
        result.score = 50;
        result.need_followup = true;
        result.feedback = "回答过短，建议补充概念定义、作用和场景。";
    } else if (answer.size() <= 30) {
        result.score = 75;
        result.need_followup = true;
        result.feedback = "回答基本覆盖了核心内容，但还可以继续展开。";
    } else {
        result.score = 90;
        result.need_followup = false;
        result.feedback = "回答较完整，表达较清楚。";
    }

    // 第一阶段先给固定风格的追问。
    // 这里根据题目内容返回不同追问，能让控制台流程更像真实面试。
    if (result.need_followup) {
        if (question.text == "请介绍一下 RAII") {
            result.followup_question = "RAII 在异常安全里有什么作用？";
        } else if (question.text == "请说一下智能指针的作用") {
            result.followup_question = "shared_ptr 和 unique_ptr 的区别是什么？";
        } else if (question.text == "请解释 epoll 和 select 的区别") {
            result.followup_question = "epoll 为什么在高并发场景下更有优势？";
        } else {
            result.followup_question = "你可以再补充一下实现原理和使用场景吗？";
        }
    }

    return result;
}

std::string MockLLMClient::GenerateSummary(const std::vector<AnswerRecord>& records) {
    std::ostringstream oss;

    int total_score = 0;
    for (const auto& record : records) {
        total_score += record.score;
    }

    int average_score = 0;
    if (!records.empty()) {
        average_score = total_score / static_cast<int>(records.size());
    }

    oss << "本次模拟面试共记录 " << records.size() << " 条回答，";
    oss << "平均分约为 " << average_score << " 分。";

    if (average_score >= 85) {
        oss << "你的整体回答比较完整，基础掌握较好。";
    } else if (average_score >= 70) {
        oss << "你的基础概念比较清楚，但还可以继续加强细节表达和场景展开。";
    } else {
        oss << "你的回答还有较大提升空间，建议先加强基础概念理解，再补充项目应用场景。";
    }

    return oss.str();
}

}  // namespace interview::services
