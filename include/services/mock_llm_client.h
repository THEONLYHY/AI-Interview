#ifndef SERVICES_MOCK_LLM_CLIENT_H_
#define SERVICES_MOCK_LLM_CLIENT_H_

#include <string>
#include <vector>

#include "llm_client.h"

namespace interview::services {

// MockLLMClient 是第一阶段使用的假 LLM 实现。
// 它不访问真实网络，也不调用真实大模型，
// 只是返回固定题目、固定规则评分和简单总结，
// 用来帮助我们先把主流程跑通。
class MockLLMClient : public LLMClient {
public:
    // 生成主问题列表。
    // 第三阶段虽然接口已经支持 resume / JD / question_count，
    // 但 mock 实现里可以先忽略 resume_text 和 job_description，
    // 仅根据 question_count 返回固定题目
    std::vector<interview::common::Question> GenerateQuestions(
        const std::string& resume_text,
        const std::string& job_description,
        int question_count) override;

    // 对用户回答进行简单评估。
    // 第三阶段接口加入了 history，用于兼容真实 LLM 的上下文输入；
    // 但 mock 版本里可以先不使用它。
    interview::common::EvaluateResult EvaluateAnswer(
        const interview::common::Question& question,
        const std::string& answer,
        const std::vector<interview::common::AnswerRecord>& history) override;

    std::string GenerateSummary(
        const std::vector<interview::common::AnswerRecord>& records) override;
};

}  // namespace interview::services

#endif //SERVICES_MOCK_LLM_CLIENT_H_
