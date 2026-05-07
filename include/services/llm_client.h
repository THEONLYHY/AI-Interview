#ifndef SERVICES_LLM_CLIENT_H_
#define SERVICES_LLM_CLIENT_H_

#include <string>
#include <vector>

#include "common/protocol.h"


class LLMClient {
public:
    virtual ~LLMClient() = default;
    /// @brief 生成主问题列表
    /// @return 一个Question数组，表示本场面试的主问题
    virtual std::vector<Question> GenerateQuestions() = 0;
    
    virtual EvaluateResult EvaluateAnswer(const Question& question, 
                                          const std::string& answer) = 0;

    virtual std::string GenerateSummary(const std::vector<AnswerRecord>& records) = 0;
};


#endif // SERVICES_LLM_CLIENT_H_
