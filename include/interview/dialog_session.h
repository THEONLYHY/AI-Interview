#ifndef INTERVIEW_DIALOGSESSION_H
#define INTERVIEW_DIALOGSESSION_H

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <thread>

#include "common/dialog_state.h"
#include "common/protocol.h"
#include "interview_session.h"
#include "services/realtime_client.h"
#include "services/audio_manager.h"

namespace interview::session {

// DialogSession：会话流程协调层
// 1. 驱动 InterviewSession（业务）
// 2. 管理 DialogState 状态机
// 3. 持有 RealtimeClient（Mock / Real）
// 4. OnServerEvent 接收 ParsedResponse（与 Protocol::ParseResponse 输出对齐）
class DialogSession {
public:
    // 内容回调供 UI/测试订阅会话文本事件。
    // role 用于区分 question/candidate/feedback/followup/summary/error 等展示样式；
    // question_index 对应主问题编号，非题目相关内容使用 -1。
    using DialogContentCallback =
        std::function<void(const std::string& role,
                           const std::string& text,
                           int question_index)>;

    // 状态回调供 UI/测试订阅 DialogState 变化。
    // 回调可能在会话、网络或音频线程触发，调用方负责切回自己的线程。
    using DialogStateCallback =
        std::function<void(interview::common::DialogState state)>;

    explicit DialogSession(
        std::unique_ptr<InterviewSession> interview_session,
        std::unique_ptr<interview::services::RealtimeClient> realtime_client,
        bool audio_enabled = false);

    DialogSession(const DialogSession&) = delete;
    DialogSession& operator=(const DialogSession&) = delete;
    DialogSession(DialogSession&&) = delete;
    DialogSession& operator=(DialogSession&&) = delete;

    ~DialogSession();

    void Start();

    // stdin 文本问答（兼容旧入口，等价于 RunFromStdin）
    void Run();

    void RunFromStdin();
    // 阻塞直到 kSessionFinished / kStopped / Stop()
    void RunEventDriven();

    void Stop();

    interview::common::DialogState State() const;

    void SetContentCallback(DialogContentCallback callback);
    void SetStateCallback(DialogStateCallback callback);

private:
    void SetState(interview::common::DialogState new_state);
    void EmitContent(const std::string& role,
                     const std::string& text,
                     int question_index);

    // 负责出题
    void OnAskQuestion();
    // 处理主问题回答
    void OnCandidateAnswer(const std::string& answer);
    void OnFollowupAnswer(const std::string& answer);
    void OnEnterSummary();
    void RequestCurrentQuestionSpeech();

    void OnServerEvent(const interview::common::ParsedResponse& evt);

    // 通过 kChatTextQuery 让服务端 TTS 读文字（阶段 7 真链路透传）
    void SpeakText(const std::string& text);

    // kAsrEnded 时提交 accumulated ASR 文本并推进题流
    void HandleAsrFinalized();

    // 音频线程入口。负责线程调度和队列同步
    bool StartAudioThreads();
    void StopAudioThread();
    void RecordingLoop();
    void PlaybackLoop();
    void EnqueueTtsAudio(const std::vector<uint8_t>& pcm);
    void MarkTtsPlaybackDrained();
    bool CanSendCandidateAudio() const;
    std::string SessionIdSnapshot() const;

    std::unique_ptr<InterviewSession> interview_session_;
    std::unique_ptr<interview::services::RealtimeClient> realtime_client_;
    std::unique_ptr<interview::services::AudioManager> audio_manager_;

    // 主线程 Run/Stop 和实时接收线程的 OnServerEvent同时访问，用锁保护
    interview::common::DialogState state_ = interview::common::DialogState::kInit;
    mutable std::mutex state_mutex_;
    std::mutex stop_mutex_;    

    // 回调由 UI/测试注册，由会话/网络/audio 线程触发；复制后再调用，避免持锁执行外部代码。
    mutable std::mutex callback_mutex_;
    DialogContentCallback content_callback_;
    DialogStateCallback state_callback_;

    // data_mutex_ 保护服务端事件携带的会话数据。
    // session_id_ 在 kSessionStarted 中写入，SpeakText/SendAudio 等发送路径读取；
    // current_asr_text_ 由 ASR 事件持续覆盖，kAsrEnded 时提交给 InterviewSession。
    mutable std::mutex data_mutex_;

    // OnServerEvent 在工作线程、Run*/Stop 在主线程，需原子避免 data race
    std::atomic<bool> is_running_{false};

    std::atomic<bool> audio_threads_running_{false};
    bool audio_enabled_ = false;

    // 录音线程只在候选人可说话窗口发送 kTaskRequest 音频帧; 
    // 播放线程只消费服务端kTtsResponse PCM 队列。
    // 二者分离后，TTS播放阻塞不会拖慢麦克风循环。
    std::thread recording_thread_;
    std::thread playback_thread_;

    // 实时接收线程生产 TTS PCM，播放线程消费。队列里保存原始 PCM bytes，
    // 到播放线程再转 float32，保证 OnServerEvent 尽快返回继续收包。
    mutable std::mutex tts_mutex_;
    std::condition_variable tts_cv_;
    std::queue<std::vector<float>> tts_queue_;
    std::vector<uint8_t> tts_decode_remainder_;

    std::string session_id_;
    std::string current_asr_text_;
};

}  // namespace interview::session

#endif  // INTERVIEW_DIALOGSESSION_H
