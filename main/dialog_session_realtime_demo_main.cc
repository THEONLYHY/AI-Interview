#include <memory>

#include "common/logger.h"
#include "interview/dialog_session.h"
#include "interview/interview_session.h"
#include "services/mock_llm_client.h"
#include "services/realtime_client.h"

using namespace interview;

// 这个 main 的目标：
// 1. 创建 InterviewSession
// 2. 创建 RealtimeClient
// 3. 创建 DialogSession
// 4. 调用 Start()，验证：
//    - DialogSession 能启动
//    - RealtimeClient 能连接
//    - DialogSession 能通过 RealtimeClient 发送一条最小 hello 消息
//
// 当前这一步先不调用 Run()，
// 因为我们现在主要测试第四阶段“会话层 -> 实时层 -> 协议层”是否已经挂上。
int main() {
  // 初始化日志系统。
  if (!common::Logger::Init()) {
    return 1;
  }

  // 先使用 MockLLMClient。
  // 当前这个 demo 不关注真实 LLM，只关注第四阶段实时层接入骨架。
  auto llm_client = std::make_unique<services::MockLLMClient>();

  // 创建 InterviewSession。
  auto interview_session =
      std::make_unique<session::InterviewSession>(std::move(llm_client));

  // 创建 RealtimeClient。
  // 当前第四阶段还没接真实 WebSocket，所以这里先给一个占位地址。
  auto realtime_client =
      std::make_unique<services::RealTimeClient>("ws://localhost:9000");

  // 创建 DialogSession，把业务层和实时层一起挂进去。
  session::DialogSession dialog_session(std::move(interview_session),
                                        std::move(realtime_client));

  // 启动会话。
  // 你当前应该能在日志里看到：
  // 1. realtime client connected
  // 2. hello message sent to realtime client
  // 3. 欢迎参加模拟面试
  dialog_session.Start();

  // 当前这个 demo 先不进入完整问答流程。
  // 后面如果你想继续测文本面试主流程，再把 Run() 打开。
  //
  // dialog_session.Run();

  // 结束会话。
  dialog_session.Stop();

  return 0;
}
