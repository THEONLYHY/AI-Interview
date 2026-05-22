#include <cassert>
#include <memory>
#include <string>

#include "interview/interview_session.h"
#include "services/mock_llm_client.h"

namespace {

using interview::session::InterviewSession;
using interview::services::MockLLMClient;

void GenerateQuestionsTruncatesWhenRequested() {
    MockLLMClient llm;
    const auto qs =
            llm.GenerateQuestions("", "job", /*question_count=*/2);
    assert(qs.size() == 2u);
    assert(qs[0].id == 1);
    assert(qs[1].id == 2);
}

void MockQuestionsStayFixedWhenResumeLoaded() {
    MockLLMClient llm;
    const auto without_resume = llm.GenerateQuestions("", "job", 3);
    const auto with_resume = llm.GenerateQuestions("resume text", "job", 3);
    assert(!without_resume.empty());
    assert(!with_resume.empty());
    assert(with_resume[0].text == without_resume[0].text);
}

void ShortAnswerTriggersFollowupFlow() {
    auto llm = std::make_unique<MockLLMClient>();
    InterviewSession session(std::move(llm));
    session.Start();

    assert(session.HasNextQuestion());

    const auto result = session.SubmitAnswer("short");
    assert(result.need_followup);
    assert(session.HasPendingFollowup());

    const auto follow_q = session.GetPendingFollowupQuestion();
    assert(follow_q.is_followup);

    const auto fu = session.SubmitFollowupAnswer(
            "long enough follow-up answer text here.");
    assert(fu.score >= 0);

    session.MoveToNextQuestion();
    assert(session.HasNextQuestion());
}

void FullInterviewWithoutFollowupProducesReport() {
    auto llm = std::make_unique<MockLLMClient>();
    InterviewSession session(std::move(llm));
    session.Start();

    const std::string long_answer =
            "This answer is intentionally verbose so MockLLM marks it "
            "complete without follow-up.";
    while (session.HasNextQuestion()) {
        const auto r = session.SubmitAnswer(long_answer);
        assert(!r.need_followup);
        session.MoveToNextQuestion();
    }

    const interview::common::InterviewReport report =
            session.GenerateReport();

    assert(report.records.size() == 3u);
    assert(report.total_score ==
           report.records[0].score + report.records[1].score +
               report.records[2].score);
    assert(!report.summary.empty());
}

}  // namespace

int main() {
    GenerateQuestionsTruncatesWhenRequested();
    MockQuestionsStayFixedWhenResumeLoaded();
    ShortAnswerTriggersFollowupFlow();
    FullInterviewWithoutFollowupProducesReport();
    return 0;
}
