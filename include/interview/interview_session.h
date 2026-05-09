#ifndef INTERVIEW_INTERVIEW_SESSION_H_
#define INTERVIEW_INTERVIEW_SESSION_H_

#include <memory>
#include <vector>

#include "common/interview_types.h"
#include "services/llm_client.h"

// 1. 启动面试并初始化题目
// 2. 提供当前主问题
// 3. 接收用户回答并调用LLM 评分
// 4. 判断是否需要追问
// 5. 保存所有回答记录
// 6. 在结束后生成最终报告
class InterviewSession {
public:
    explicit InterviewSession(std::unique_ptr<LLMClient> llm_client);

    void Start();

    bool HasNextQuestion() const;

    Question GetCurrentQuestion() const;

    EvaluateResult SubmitAnswer(const std::string& answer);

    bool HasPendingFollowup() const;

    Question GetPendingFollowupQuestion() const;

    EvaluateResult SubmitFollowupAnswer(const std::string& answer);

    void MoveToNextQuestion();

    InterviewReport GenerateReport() const;
private:
    // 
    std::unique_ptr<LLMClient> llm_client_;
    // 主问题列表
    std::vector<Question> questions_;
    // 整场面试的所有回答记录
    std::vector<AnswerRecord> records_;

    int current_index_ = 0; // 当前进行到第几题

    bool started_ = false;
    // 当主问题回答后，是否还有待处理的追问
    bool has_pending_followup_ = false;
    // 当前缓存的待处理的追问题目
    Question pending_followup_question_;
};


#endif // INTERVIEW_INTERVIEW_SESSION_H_