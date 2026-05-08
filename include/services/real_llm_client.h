#ifndef SERVICES_REAL_LLM_CLIENT_H_
#define SERVICES_REAL_LLM_CLIENT_H_

#include <string>
#include <vector>

#include "llm_client.h"
// RealLLMClient 是第三阶段新增的真实大模型实现。
// 它的职责是：
// 1. 组织 prompt
// 2. 发起真实模型调用
// 3. 解析结构化返回结果
// 4. 在失败场景下提供降级处理
//
// 第三阶段建议先从 EvaluateAnswer() 的真实接入开始，
// GenerateQuestions() 和 GenerateSummary() 可以先保留简单实现
// 或暂时回退到固定逻辑。
class RealLLMClient : public LLMClient {
public:
    RealLLMClient(std::string api_url,
                    std::string api_key,
                    std::string model_name,
                    double temperature,
                    int max_tokens,
                    int timeout_seconds);

    // 生成主问题列表。
    // 第三阶段可以先做一个最小版本：
    // 1. 先回退到固定题目
    // 2. 或者后面再接真实题目生成
    std::vector<Question> GenerateQuestions(
        const std::string& resume_text,
        const std::string& job_description,
        int question_count) override;
    
    // 评估回答。
    // 这是第三阶段最推荐优先接入真实能力的接口。
    EvaluateResult EvaluateAnswer(
        const Question& question,
        const std::string& answer,
        const std::vector<AnswerRecord>& history) override;

        // 生成整场面试总结。
    // 第三阶段初期可以先做一个保底实现，
    // 等真实评分稳定后再切到真实总结生成。
    std::string GenerateSummary(
        const std::vector<AnswerRecord>& records) override;
private:
    // 构造“回答评估”用的 prompt。
    // 作用：
    //   把当前题目、当前回答、历史记录组织成模型输入。
    std::string BuildEvaluatePrompt(
        const Question& question,
        const std::string& answer,
        const std::vector<AnswerRecord>& history) const;
    
    // 构造整场面试总结 prompt。
    std::string BuildSummaryPrompt(
        const std::vector<AnswerRecord>& records) const;

    // 构造题目生成用的 prompt。
    // 参数：
    //   resume_text: 简历文本
    //   job_description: 岗位描述
    //   question_count: 希望生成的题目数量
    //
    // 返回值：
    //   发送给大模型的 prompt 文本。
    std::string BuildQuestionsPrompt(
        const std::string& resume_text,
        const std::string& job_description,
        int question_count) const;

    // 解析题目生成结果。
    // 参数：
    //   response_text: 模型返回的 message.content，要求是一段 JSON 文本
    //
    // 返回值：
    //   解析后的题目列表。
    //   如果解析失败，调用方应走 fallback 固定题目。
    std::vector<Question> ParseQuestions(const std::string& response_text) const;
    // 解析模型返回的结构化结果。
    // 第三阶段建议模型输出 JSON，再由这里解析成 EvaluateResult。
    EvaluateResult ParseEvaluateResult(const std::string& response_text) const;

    // 真实模型调用接口。
    // 第三阶段先把它抽出来，后面可以在 .cc 里接：
    // 1. HTTP 请求
    // 2. SDK 调用
    // 3. 本地推理服务
    std::string CallModel(const std::string& prompt) const;

    // 当真实模型调用失败时，提供兜底评估结果。
    EvaluateResult BuildFallbackEvaluateResult() const;

    // 当真实总结失败时，提供兜底总结。
    std::string BuildFallbackSummary(
        const std::vector<AnswerRecord>& records) const;

private:
    // 直接保存完整请求地址，比如：
    // 
    std::string api_url_;
    // Bearer Token
    std::string api_key_;
    // 模型名，比如qwen3-8b
    std::string model_name_;
    // 温度参数
    double temperature_ = 0.3;

    int max_tokens_ = 32000;

    int timeout_seconds_ = 60;
};

#endif // SERVICES_REAL_LLM_CLIENT_H_