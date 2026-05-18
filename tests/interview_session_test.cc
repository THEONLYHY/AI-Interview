#include <memory>
#include <string>

#include <gtest/gtest.h>

#include "interview/interview_session.h"
#include "services/mock_llm_client.h"

namespace {

using interview::session::InterviewSession;
using interview::services::MockLLMClient;

TEST(MockLLMClient, GenerateQuestionsTruncatesWhenRequested) {
    MockLLMClient llm;
    const auto qs =
            llm.GenerateQuestions("", "job", /*question_count=*/2);
    ASSERT_EQ(qs.size(), 2u);
    EXPECT_EQ(qs[0].id, 1);
    EXPECT_EQ(qs[1].id, 2);
}

TEST(InterviewSessionMock, ShortAnswerTriggersFollowupFlow) {
    auto llm = std::make_unique<MockLLMClient>();
    InterviewSession session(std::move(llm));
    session.Start();

    ASSERT_TRUE(session.HasNextQuestion());

    const auto result = session.SubmitAnswer("short");
    ASSERT_TRUE(result.need_followup);
    ASSERT_TRUE(session.HasPendingFollowup());

    const auto follow_q = session.GetPendingFollowupQuestion();
    EXPECT_TRUE(follow_q.is_followup);

    const auto fu = session.SubmitFollowupAnswer(
            "long enough follow-up answer text here.");
    EXPECT_GE(fu.score, 0);

    session.MoveToNextQuestion();
    EXPECT_TRUE(session.HasNextQuestion());
}

TEST(InterviewSessionMock, FullInterviewWithoutFollowupProducesReport) {
    auto llm = std::make_unique<MockLLMClient>();
    InterviewSession session(std::move(llm));
    session.Start();

    const std::string long_answer =
            "This answer is intentionally verbose so MockLLM marks it "
            "complete without follow-up.";
    while (session.HasNextQuestion()) {
        const auto r = session.SubmitAnswer(long_answer);
        EXPECT_FALSE(r.need_followup);
        session.MoveToNextQuestion();
    }

    const interview::common::InterviewReport report =
            session.GenerateReport();

    ASSERT_EQ(report.records.size(), 3u);
    EXPECT_EQ(report.total_score,
              report.records[0].score + report.records[1].score +
                      report.records[2].score);
    EXPECT_FALSE(report.summary.empty());
}

}  // namespace
