#ifndef SERVICES_MOCK_LLM_CLIENT_H_
#define SERVICES_MOCK_LLM_CLIENT_H_

#include <string>
#include <vector>

#include "llm_client.h"

// MockLLMClient 是第一阶段使用的假 LLM 实现。
// 它不访问真实网络，也不调用真实大模型，
// 只是返回固定题目、固定规则评分和简单总结，
// 用来帮助我们先把主流程跑通。
class MockLLMClient : public LLMClient {
public:
    // 生成本场面试的主问题列表
    std::vector<Question> GenerateQuestions() override;

    EvaluateResult EvaluateAnswer(const Question& question, 
                                          const std::string& answer) override;

    std::string GenerateSummary(const std::vector<AnswerRecord>& records) override;                        

};

#endif //SERVICES_MOCK_LLM_CLIENT_H_