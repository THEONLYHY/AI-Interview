#ifndef SERVICES_LLM_CLIENT_H_
#define SERVICES_LLM_CLIENT_H_

#include <string>
#include <vector>

#include "common/interview_types.h"

namespace interview::services {

class LLMClient {
public:
    virtual ~LLMClient() = default;
    // 生成主问题列表。
    // 参数：
    //   resume_text: 简历内容
    //   job_description: 岗位描述
    //   question_count: 需要生成多少道主问题
    //
    // 返回值：
    //   本场面试的主问题列表。
    //
    // 第三阶段开始，这个接口预留成“真实题目生成”的形式。
    // 如果当前还没接真实题目生成，MockLLMClient 或 RealLLMClient
    // 也可以先忽略部分参数，返回固定题目。
    virtual std::vector<interview::common::Question> GenerateQuestions(
        const std::string& resume_text,
        const std::string& job_description,
        int question_count
    ) = 0;
    // 评估一道题的回答。
    // 参数：
    //   question: 当前题目
    //   answer: 当前回答
    //   history: 历史回答记录，用于提供上下文
    //
    // 返回值：
    //   EvaluateResult，包含分数、是否追问、追问内容和反馈。
    //
    // 第三阶段建议优先先把这个接口接到真实 LLM。
    virtual interview::common::EvaluateResult EvaluateAnswer(
        const interview::common::Question& question,
        const std::string& answer,
        const std::vector<interview::common::AnswerRecord>& history) = 0;

    virtual std::string GenerateSummary(
        const std::vector<interview::common::AnswerRecord>& records) = 0;
};

}  // namespace interview::services

#endif // SERVICES_LLM_CLIENT_H_
