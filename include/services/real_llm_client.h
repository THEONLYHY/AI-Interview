#ifndef SERVICES_REAL_LLM_CLIENT_H_
#define SERVICES_REAL_LLM_CLIENT_H_

#include <string>
#include <vector>
#include <memory>

#include "llm_client.h"

namespace interview::services {

// RealLLMClient 是新增的真实大模型实现。
// 它的职责是：
// 1. 组织 prompt
// 2. 发起真实模型调用
// 3. 解析结构化返回结果
// 4. 在失败场景下提供降级处理
//
class RealLLMClient : public LLMClient {
public:
    RealLLMClient(std::string api_url,
                    std::string api_key,
                    std::string model,
                    double temperature,
                    int max_tokens,
                    int timeout_seconds);
    ~RealLLMClient() override;

    RealLLMClient(const RealLLMClient&) = delete;
    RealLLMClient& operator=(const RealLLMClient&) = delete;
    // 生成主问题列表。
    // 第三阶段可以先做一个最小版本：
    // 1. 先回退到固定题目
    // 2. 或者后面再接真实题目生成
    std::vector<interview::common::Question> GenerateQuestions(
        const std::string& resume_text,
        const std::string& job_description,
        int question_count) override;

    // 评估回答。
    // 这是第三阶段最推荐优先接入真实能力的接口。
    interview::common::EvaluateResult EvaluateAnswer(
        const interview::common::Question& question,
        const std::string& answer,
        const std::vector<interview::common::AnswerRecord>& history) override;

    // 生成整场面试总结。
    // 第三阶段初期可以先做一个保底实现，
    // 等真实评分稳定后再切到真实总结生成。
    std::string GenerateSummary(
        const std::vector<interview::common::AnswerRecord>& records) override;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace interview::services

#endif // SERVICES_REAL_LLM_CLIENT_H_
