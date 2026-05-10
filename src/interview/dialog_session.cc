#include "interview/dialog_session.h"
#include "common/interview_types.h"
#include "common/logger.h"

#include <iostream>

namespace interview::session {

using interview::common::DialogState;
using interview::common::DialogStateToString;
using interview::common::EvaluateResult;
using interview::common::InterviewReport;
using interview::common::MessageType;
using interview::common::Protocol;
using interview::common::ProtocolMessage;
using interview::common::Question;
using interview::common::SerializationType;
using interview::common::kProtocolVersion;
using interview::services::RealTimeClient;

DialogSession::DialogSession(std::unique_ptr<InterviewSession> interview_session,
                             std::unique_ptr<RealTimeClient> realtime_client)
: interview_session_(std::move(interview_session))
, realtime_client_(std::move(realtime_client)) {

}

void DialogSession::Start() {
    if (is_running_) {
        return;
    }
    is_running_ = true;
    SetState(DialogState::kConnecting);
    interview_session_->Start();

    // 如果有realtime_client 就先注册消息回调，再尝试连接
    if (realtime_client_) {
        realtime_client_->SetMessageHandler([this](const ProtocolMessage& message) {
            OnRealTimeMessage(message);
        });

        bool connected = realtime_client_->Connect();
        if (!connected) {
            LOG_WARN("realtime client connect failed");
        } else {
            LOG_INFO("realtime client connected");
            SendHelloMessage();
            SimulateIncomingMessage();
        }
    }

    OnInterviewStarted();
}

void DialogSession::Run() {
    if (!is_running_) {
        LOG_WARN("dialog session is not running, call Start() first");
        return;
    }
    while (is_running_ && interview_session_->HasNextQuestion()) {
        OnAskQuestion();

        SetState(DialogState::kCandidateSpeaking);

        std::string answer;
        std::getline(std::cin, answer);
        OnCandidateAnswer(answer);

        if (!is_running_) {
            break;
        }

        if (interview_session_->HasPendingFollowup()) {
            SetState(DialogState::kCandidateSpeaking);

            std::string followup_answer;
            std::getline(std::cin, followup_answer);

            OnFollowupAnswewr(followup_answer);

            if (!is_running_) {
                break;
            }
        }
        interview_session_->MoveToNextQuestion();
        SetState(DialogState::kIdle);
    }

    if (is_running_ && state_ != DialogState::kStopped) {
        OnEnterSummary();
    }
}

void DialogSession::Stop() {
    is_running_ = false;
    SetState(DialogState::kStopped);
    LOG_INFO("dialog session stopped");
}

DialogState DialogSession::state() const {
    return state_;
}

void DialogSession::SetState(DialogState new_state) {
    LOG_DEBUG("dialog state changed: {} -> {}",
              DialogStateToString(state_),
              DialogStateToString(new_state));
    state_ = new_state;
}

void DialogSession::OnInterviewStarted() {
    if (interview_session_ == nullptr) {
        LOG_WARN("interview session is nullptr");
        Stop();
        return;
    }

    SetState(DialogState::kInterviewerSpeaking);
    LOG_INFO("欢迎参加模拟面试");
    SetState(DialogState::kIdle);
}
void DialogSession::OnAskQuestion() {
    Question question = interview_session_->GetCurrentQuestion();
    SetState(DialogState::kInterviewerSpeaking);
    LOG_INFO("第{}题：{}", question.id, question.text);
}

void DialogSession::OnCandidateAnswer(const std::string& answer) {
    SetState(DialogState::kInterviewerThinking);
    EvaluateResult result = interview_session_->SubmitAnswer(answer);

    LOG_INFO("评分：{}", result.score);
    LOG_INFO("反馈：{}", result.feedback);

    if (result.need_followup) {
        SetState(DialogState::kInterviewerSpeaking);
        LOG_INFO("追问：{}", result.followup_question);
    }
}

void DialogSession::OnFollowupAnswewr(const std::string& answer) {
    SetState(DialogState::kInterviewerThinking);

    EvaluateResult result = interview_session_->SubmitFollowupAnswer(answer);

    LOG_INFO("追问评分：{}", result.score);
    LOG_INFO("追问反馈：{}", result.feedback);
}

void DialogSession::OnEnterSummary() {
    SetState(DialogState::kSessionEnding);

    InterviewReport report = interview_session_->GenerateReport();

    LOG_INFO("面试结束");
    LOG_INFO("总分：{}", report.total_score);
    LOG_INFO("总结：{}", report.summary);

    is_running_ = false;
    SetState(DialogState::kCompleted);
}

void DialogSession::OnRealTimeMessage(const ProtocolMessage& message) {
    std::string payload_text(message.payload.begin(), message.payload.end());

    switch (message.header.message_type) {
        case MessageType::kServerEvent:
            LOG_INFO("recevied server event: payload_size={}, payload={}",
                    message.payload.size(),
                    payload_text);
            break;
        case MessageType::kAuidoData:
            LOG_INFO("recevied audio data: payload_size={}",
                    message.payload.size());
            break;
        case MessageType::kError:
            LOG_ERROR("received realtime error: payload_size={}, payload={}",
                    message.payload.size(),
                    payload_text);
            break;
        default:
            LOG_WARN("received unkown realtime message: type={}, payload_size={}, payload={}",
                    static_cast<int>(message.header.message_type),
                    message.payload.size(),
                    payload_text);
            break;
    }
}

// 发送一条最小测试消息。
// 当前第四阶段先不追求真实业务消息，
// 只验证：
// 1. DialogSession 能驱动 RealtimeClient
// 2. RealtimeClient 能接到 ProtocolMessage
// 3. RealtimeClient 内部会调用 Protocol::Encode()
void DialogSession::SendHelloMessage() {
    if (!realtime_client_) {
        return;
    }

    ProtocolMessage message;
    message.header.version = kProtocolVersion;
    message.header.header_size = 1;
    message.header.message_type = MessageType::kClientEvent;
    message.header.flags = 0;
    message.header.serialization = SerializationType::kJson;

    const std::string text = "{\"event\":\"hello\"}";
    message.payload.assign(text.begin(), text.end());

    bool sent = realtime_client_->SenMessage(message);
    if (sent) {
        LOG_INFO("hello message sent to realtime client");
    } else {
        LOG_WARN("failed to send hello message");
    }
}

// 模拟接收一条服务端响应消息。
// 当前第四阶段先不接真实网络读取，
// 这里手工构造一条“服务端消息”，验证接收链路是否成立。
void DialogSession::SimulateIncomingMessage() {
    if (!realtime_client_) {
        return;
    }

    // 构造一条最小服务端响应消息。
    ProtocolMessage message;
    message.header.version = kProtocolVersion;
    message.header.header_size = 1;
    message.header.message_type = MessageType::kServerEvent;
    message.header.flags = 0;
    message.header.serialization = SerializationType::kJson;

    const std::string text = "{\"event\":\"server_ack\"}";
    message.payload.assign(text.begin(), text.end());

    // 先编码成原始字节流，模拟“网络收到的二进制数据”。
    std::vector<uint8_t> raw_data = Protocol::Encode(message);

    // 再交给 RealtimeClient 走接收解析流程。
    ProtocolMessage decoded_message;
    bool received = realtime_client_->RecvMessage(raw_data, decoded_message);

    if (received) {
        LOG_INFO("simulated incoming message received successfully");
    } else {
        LOG_WARN("failed to simulate incoming message");
    }
}

}  // namespace interview::session
