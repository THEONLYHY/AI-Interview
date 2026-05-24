#ifndef INTERVIEW_DIALOGSESSION_H
#define INTERVIEW_DIALOGSESSION_H

#include <atomic>
#include <cstddef>
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

    // 注入业务会话和实时客户端，便于真实运行和单元测试共用同一协调层。
    explicit DialogSession(
        std::unique_ptr<InterviewSession> interview_session,
        std::unique_ptr<interview::services::RealtimeClient> realtime_client,
        bool audio_enabled = false);

    // 会话对象持有线程和外部连接，不允许复制。
    DialogSession(const DialogSession&) = delete;
    DialogSession& operator=(const DialogSession&) = delete;
    DialogSession(DialogSession&&) = delete;
    DialogSession& operator=(DialogSession&&) = delete;

    // 析构时通过 Stop() 统一停止音频线程并关闭实时连接。
    ~DialogSession();

    // 初始化会话、连接实时客户端，并在语音模式下启动音频线程。
    void Start();

    // 阻塞直到 kSessionFinished / kStopped / Stop()。
    void RunEventDriven();

    // 停止会话、音频线程和实时连接；可重复调用。
    void Stop();

    // 线程安全读取当前 DialogState。
    interview::common::DialogState State() const;

    // 注册内容回调，用于 UI/测试接收问题、候选人回答、反馈等文本。
    void SetContentCallback(DialogContentCallback callback);
    // 注册状态回调，用于 UI/测试接收状态机变化。
    void SetStateCallback(DialogStateCallback callback);

private:
    // 设置状态并在锁外派发状态回调。
    void SetState(interview::common::DialogState new_state);
    // 派发文本内容；空文本会被忽略。
    void EmitContent(const std::string& role,
                     const std::string& text,
                     int question_index);

    // 负责出题
    // 文本模式下展示当前主问题。
    void OnAskQuestion();
    // 处理主问题回答
    // 文本模式下处理主问题回答并决定是否追问。
    void OnCandidateAnswer(const std::string& answer);
    // 文本模式下处理追问回答。
    void OnFollowupAnswer(const std::string& answer);
    // 生成最终报告并结束会话。
    void OnEnterSummary();
    // 将当前题目发送给服务端 TTS 朗读。
    void RequestCurrentQuestionSpeech();

    // 处理实时客户端解析后的服务端事件。
    void OnServerEvent(const interview::common::ParsedResponse& evt);

    // 通过 kChatTextQuery 让服务端 TTS 读文字（阶段 7 真链路透传）
    // 只负责请求服务端朗读；本地播放由后续 TTS 音频事件驱动。
    void SpeakText(const std::string& text);

    // kAsrEnded 时提交 accumulated ASR 文本并推进题流
    // 候选人语音结束后提交 ASR 文本并推进题目/追问/总结。
    void HandleAsrFinalized();

    // 音频线程入口。负责线程调度和队列同步
    // 打开 PortAudio 输入/输出流，并启动录音与播放线程。
    bool StartAudioThreads();
    // 停止录音/播放线程并清空 TTS 队列。
    void StopAudioThread();
    // 从麦克风读取 PCM16，必要时静音，再发送给实时客户端。
    void RecordingLoop();
    // 从 TTS 队列读取 float32 样本并写入扬声器。
    void PlaybackLoop();
    // 把服务端 TTS bytes 解码成 float32 样本并入队。
    void EnqueueTtsAudio(const std::vector<uint8_t>& pcm);
    // 在服务端 TTS_END 且本地队列播放完后打开候选人说话窗口。
    void MarkTtsPlaybackDrained();
    // 复制当前 session_id，避免发送音频时长时间持有 data_mutex_。
    std::string SessionIdSnapshot() const;

    // 题目/评分/追问/报告业务对象。
    std::unique_ptr<InterviewSession> interview_session_;
    // 实时传输对象，可是真实 WSS，也可以是测试 Mock。
    std::unique_ptr<interview::services::RealtimeClient> realtime_client_;
    // 语音模式下才创建的 PortAudio 管理对象。
    std::unique_ptr<interview::services::AudioManager> audio_manager_;

    // 主线程 Run/Stop 和实时接收线程的 OnServerEvent同时访问，用锁保护
    // 当前会话状态，所有读写都通过 state_mutex_ 串行化。
    interview::common::DialogState state_ = interview::common::DialogState::kInit;
    // 保护 state_，避免 UI/网络/主线程同时读写状态。
    mutable std::mutex state_mutex_;
    // 防止 StopAudioThread() 被 Stop()、事件线程和错误路径同时执行。
    std::mutex stop_mutex_;    

    // 回调由 UI/测试注册，由会话/网络/audio 线程触发；复制后再调用，避免持锁执行外部代码。
    mutable std::mutex callback_mutex_;
    // 文本内容回调，注册后可能从多个工作线程触发。
    DialogContentCallback content_callback_;
    // 状态变化回调，注册时会立即回放当前状态。
    DialogStateCallback state_callback_;

    // data_mutex_ 保护服务端事件携带的会话数据。
    // session_id_ 在 kSessionStarted 中写入，SpeakText/SendAudio 等发送路径读取；
    // current_asr_text_ 由 ASR 事件持续覆盖，kAsrEnded 时提交给 InterviewSession。
    mutable std::mutex data_mutex_;

    // OnServerEvent 在工作线程、Run*/Stop 在主线程，需原子避免 data race
    // 会话主运行标志，控制 stdin/event/audio 循环退出。
    std::atomic<bool> is_running_{false};

    // 音频线程运行标志，专门控制录音/播放线程生命周期。
    std::atomic<bool> audio_threads_running_{false};
    // TTS 播放门控：播放时录音线程发送静音，播放完才恢复真实麦克风。
    std::atomic<bool> is_playing_audio_{false};
    // 构造参数保存的语音模式开关。
    bool audio_enabled_ = false;

    // 录音线程只在候选人可说话窗口发送 kTaskRequest 音频帧; 
    // 播放线程只消费服务端kTtsResponse PCM 队列。
    // 二者分离后，TTS播放阻塞不会拖慢麦克风循环。
    std::thread recording_thread_;
    // 播放线程句柄，由 StopAudioThread() join。
    std::thread playback_thread_;

    // 实时接收线程生产 TTS PCM，播放线程消费。队列里保存原始 PCM bytes，
    // 到播放线程再转 float32，保证 OnServerEvent 尽快返回继续收包。
    mutable std::mutex tts_mutex_;
    // 唤醒播放线程消费新入队的 TTS 样本。
    std::condition_variable tts_cv_;
    // 待播放 TTS 样本队列；每个元素是一段 float32 PCM。
    std::queue<std::vector<float>> tts_queue_;
    // 保存跨包残留的未满 4 字节 float32 尾巴。
    std::vector<uint8_t> tts_decode_remainder_;
    // 已出队但尚未完成 WriteAudio 的 TTS 块数量。
    std::size_t tts_playback_in_flight_ = 0;
    // 服务端 TTS_END 是否已经到达；本地播放还需等待队列清空。
    bool tts_end_received_ = false;

    // 服务端 kSessionStarted 下发的会话 id，发送文本/音频时需要携带。
    std::string session_id_;
    // 最新累计 ASR 文本，kAsrEnded 时提交给 InterviewSession。
    std::string current_asr_text_;
};

}  // namespace interview::session

#endif  // INTERVIEW_DIALOGSESSION_H
