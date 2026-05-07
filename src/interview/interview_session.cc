#include "interview/interview_session.h"


InterviewSession::InterviewSession(std::unique_ptr<LLMClient> llm_client)
: llm_client_(std::move(llm_client)){

}

void InterviewSession::Start() {
    questions_ = llm_client_->GenerateQuestions();

    current_index_ = 0;
    started_ = true;

    has_pending_followup_ = false;
    pending_followup_question_ = Question{};

}

bool InterviewSession::HasNextQuestion() const {
    return started_ && current_index_ >= 0 && current_index_ < static_cast<int>(questions_.size());
}

Question InterviewSession::GetCurrentQuestion() const {
    return questions_[current_index_];
}

// 提交当前主问题的回答。
// 主要做三件事：
// 1. 获取当前主问题
// 2. 调用 LLMClient 评分
// 3. 保存本次回答记录，并根据评分结果决定是否设置追问
EvaluateResult InterviewSession::SubmitAnswer(const std::string& answer) {
    Question current_question = GetCurrentQuestion();
    EvaluateResult result = llm_client_->EvaluateAnswer(current_question, answer);

    AnswerRecord record;
    record.question_id = current_question.id;
    record.question_text = current_question.text;
    record.answer_text = answer;
    record.score = result.score;
    record.need_followup = result.need_followup;
    record.followup_question = result.followup_question;
    record.is_followup_answer = false;

    records_.push_back(record);
    if (result.need_followup) {
        has_pending_followup_ = true;
        pending_followup_question_.id = current_question.id;
        pending_followup_question_.text = result.followup_question;
        pending_followup_question_.is_followup = true;
        pending_followup_question_.parent_question_id = current_question.id;
    } else {
        has_pending_followup_ = false;
        pending_followup_question_ = Question{};
    }
    return result;
}

bool InterviewSession::HasPendingFollowup() const {
    return has_pending_followup_;
}

Question InterviewSession::GetPendingFollowupQuestion() const {
    return pending_followup_question_;
}

// 提交追问回答。
// 第一阶段这里也复用 LLMClient::EvaluateAnswer 做简单评分。
// 但提交追问回答后，不再继续产生新的追问，避免第一阶段流程变复杂。
EvaluateResult InterviewSession::SubmitFollowupAnswer(const std::string& answer) {
    Question followup_question = GetPendingFollowupQuestion();

    EvaluateResult result = llm_client_->EvaluateAnswer(followup_question, answer);

    AnswerRecord record;
    record.question_id = followup_question.parent_question_id;
    record.question_text = followup_question.text;
    record.answer_text = answer;
    record.score = result.score;
    record.need_followup = false;
    record.followup_question.clear();
    record.is_followup_answer = true;

    return result;
}

void InterviewSession::MoveToNextQuestion() {
    ++current_index_;

    has_pending_followup_ = false;
    pending_followup_question_ = Question{};
}

// 生成整场面试报告。
// 主要做三件事：
// 1. 拷贝所有回答记录
// 2. 统计总分
// 3. 调用 LLMClient 生成总结文本
InterviewReport InterviewSession::GenerateReport() const {
    InterviewReport report;
    report.records = records_;

    int total_score = 0;
    for (const auto& record : records_) {
        total_score += record.score;
    }

    report.total_score = total_score;
    report.summary = llm_client_->GenerateSummary(records_);

    return report;
}