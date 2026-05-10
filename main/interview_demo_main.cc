#include "interview/interview_session.h"
#include "services/mock_llm_client.h"
#include "common/logger.h"

#include <iostream>
#include <memory>

using namespace interview;

// 第一阶段最小控制台入口。
// 负责：
// 1. 创建 MockLLMClient
// 2. 创建 InterviewSession
// 3. 启动面试
// 4. 循环读取用户输入并提交
// 5. 打印评分、追问和最终总结
int main() {
    if (!common::Logger::Init()) {
        std::cerr << "logger init failed\n";
        return 1;
    }

    auto llm_client = std::make_unique<services::MockLLMClient>();
    session::InterviewSession isession(std::move(llm_client));

    isession.Start();
    std::cout << "[系统] 欢迎参加模拟面试\n\n";

    while (isession.HasNextQuestion()) {
        common::Question question = isession.GetCurrentQuestion();

        // 打印题目。
        std::cout << "[系统] 第" << question.id << "题：" << question.text
                  << "\n";
        std::cout << "[你] ";

        std::string answer;
        std::getline(std::cin, answer);

        common::EvaluateResult result = isession.SubmitAnswer(answer);

        // 打印评分和反馈。
        std::cout << "\n[系统] 评分：" << result.score << "\n";
        std::cout << "[系统] 反馈：" << result.feedback << "\n";

        if (isession.HasPendingFollowup()) {
            common::Question followup_question = isession.GetPendingFollowupQuestion();

            std::cout << "[系统] 追问：" << followup_question.text << "\n";
            std::cout << "[你] ";

            std::string followup_answer;
            std::getline(std::cin, followup_answer);

            // 提交追问回答。
            common::EvaluateResult followup_result =
                isession.SubmitFollowupAnswer(followup_answer);

            // 打印追问回答的评分和反馈。
            std::cout << "\n[系统] 追问评分：" << followup_result.score << "\n";
            std::cout << "[系统] 追问反馈：" << followup_result.feedback
                      << "\n";
        }
        // 当前主问题及其追问处理完成后，进入下一题。
        isession.MoveToNextQuestion();

        // 题与题之间空一行，方便控制台阅读。
        std::cout << "\n[系统] 进入下一题\n\n";
    }
        // 所有题目结束后，生成整场面试报告。
    common::InterviewReport report = isession.GenerateReport();

    std::cout << "[系统] 面试结束\n";
    std::cout << "[系统] 总分：" << report.total_score << "\n";
    std::cout << "[系统] 总结：" << report.summary << "\n";

    return 0;
}
