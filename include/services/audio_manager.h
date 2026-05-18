#ifndef INCLUDE_SERVICES_AUDIO_MANAGER_H_
#define INCLUDE_SERVICES_AUDIO_MANAGER_H_

#include <cstdint>
#include <memory>
#include <vector>

namespace interview::services {

// AudioManager 是项目中唯一直接与 PortAudio 交互的类。
//
// 在 Stage 8 阶段，这个音频封装类被刻意设计得比较小，
// 只负责最底层、最基础的音频输入输出操作：
//
// 1. 输入部分：
//    - 使用阻塞式方式采集麦克风音频；
//    - 返回的数据格式为：
//      mono 单声道、16 kHz 采样率、int16 小端序 PCM 字节流；
//    - 这些字节数据可以直接发送给 RealtimeClient::SendAudio()。
//
// 2. 输出部分：
//    - 使用阻塞式方式播放 TTS 返回的音频；
//    - TTS 音频格式为：
//      mono 单声道、24 kHz 采样率、int16 小端序 PCM 字节流；
//    - 播放前会转换为 float32，以便交给 PortAudio 输出。
//
// 3. 线程与队列：
//    - AudioManager 不负责高层录音线程、播放线程或音频队列；
//    - 这些更高层的控制逻辑由 DialogSession 负责；
//    - AudioManager 只提供“开始输入 / 读取音频 / 停止输入”
//      以及“开始输出 / 播放音频 / 停止输出”等基础能力。
//
// 4. Pimpl 设计：
//    - 本类使用 Pimpl（Pointer to Implementation）模式；
//    - 这样 public 头文件中不需要暴露 portaudio.h；
//    - 可以减少其他模块的编译依赖；
//    - 也方便后续与 Qt 等 GUI 框架集成时降低耦合。

class AudioManager {
public:
    AudioManager();
    ~AudioManager();

    AudioManager(const AudioManager&) = delete;
    AudioManager& operator=(const AudioManager&) = delete;

    AudioManager(AudioManager&&) = delete;
    AudioManager& operator=(AudioManager&&) = delete;

    // 打开默认输入设备。
    //
    // 通常对应系统默认麦克风。
    //
    // 成功后，可以通过 ReadInputChunk() 读取音频块。
    // ReadInputChunk() 返回的音频数据是原始 PCM 字节，
    // 可直接传给 RealtimeClient::SendAudio()。
    //
    // 返回值：
    // - true：输入设备打开成功；
    // - false：输入设备打开失败，例如没有麦克风、设备被占用、
    //          PortAudio 初始化失败等。
    bool StartInput();
    // 从输入流中读取一个配置好的音频块。
    //
    // 参数：
    // - pcm：输出参数，用于接收读取到的 PCM 音频字节。
    //
    // 音频格式：
    // - 单声道 mono；
    // - 16 kHz 采样率；
    // - int16；
    // - little-endian 小端序；
    // - 原始 PCM 字节流。
    //
    // 返回值：
    // - true：成功读取到一块音频数据；
    // - false：发生一次临时读取失败。
    //
    // 注意：
    // false 并不一定表示录音需要终止。
    // DialogSession 会把它当作类似 EAGAIN 的临时失败处理，
    // 通常会短暂 sleep 后继续重试，而不是直接停止整个会话。
    bool ReadInputChunk(std::vector<uint8_t>& pcm);
    // 停止输入流。
    //
    // 关闭已经打开的默认输入设备，
    // 释放麦克风相关资源。
    //
    // 如果输入流没有打开，调用该函数通常应当是安全的。
    void StopInput();
    // 打开默认输出设备。
    //
    // 通常对应系统默认扬声器或耳机。
    //
    // 虽然服务端返回的 TTS 音频是 int16 PCM，
    // 但这里会以 float32 方式打开 PortAudio 输出流。
    //
    // 使用 float32 输出的原因：
    // - 避免不同平台上 int16 输出时可能出现的裁剪差异；
    // - int16 转 float32 归一化后更容易控制音量范围；
    // - PortAudio 对 float32 输出支持较好，跨平台表现更稳定。
    //
    // 返回值：
    // - true：输出设备打开成功；
    // - false：输出设备打开失败，例如没有可用扬声器、
    //          设备被占用或 PortAudio 打开流失败。
    bool StartOutput();
    // 播放一段 TTS PCM 音频数据。
    //
    // 参数：
    // - pcm：服务端返回的 TTS 音频数据。
    //
    // 输入音频格式：
    // - 单声道 mono；
    // - 24 kHz 采样率；
    // - int16；
    // - little-endian 小端序；
    // - 原始 PCM 字节流。
    //
    // 内部处理流程：
    // 1. 将 int16 little-endian PCM 转换为 float32；
    // 2. 将采样值归一化到 [-1.0, 1.0) 范围；
    // 3. 使用 PortAudio 输出流进行阻塞式播放。
    //
    // 返回值：
    // - true：播放成功；
    // - false：播放失败，例如输出流未打开、写入失败等。
    //
    // 注意：
    // 该调用是阻塞式的。
    // DialogSession 会在独立的播放线程中调用它，
    // 避免阻塞主会话逻辑。
    bool PlayTtsPcm(const std::vector<uint8_t>& pcm);
    // 停止输出流
    // 关闭已经打开的默认输出设备
    // 释放扬声器或耳机相关资源
    // 如果输出流没有打开，调用该函数通应当是安全的
    void StopOutput();

    // PCM int16 小端序转 float32 的纯转换辅助函数。
    //
    // 该函数保持 public，主要是为了方便无硬件环境下的单元测试。
    // 例如测试时不需要真实麦克风或扬声器，
    // 只需要验证 PCM 转换逻辑是否正确。
    //
    // 参数：
    // - pcm：输入的 PCM 字节数据。
    //
    // 输入格式：
    // - little-endian 小端序；
    // - signed 16-bit PCM；
    // - 每两个字节表示一个 int16 采样点。
    //
    // 输出格式：
    // - std::vector<float>；
    // - 每个采样点被归一化到 [-1.0, 1.0) 范围。
    //
    // 典型转换规则：
    // - int16 最小值 -32768 转换为 -1.0；
    // - int16 最大值  32767 转换为接近 1.0；
    // - 0 转换为 0.0。
    //
    // 注意：
    // 如果 pcm 的字节数不是 2 的倍数，
    // 实现中通常应忽略最后一个不完整字节，
    // 或根据项目约定进行错误处理。
    static std::vector<float> Pcm16LeToFloat32(const std::vector<uint8_t>& pcm);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace interview::services

#endif // INCLUDE_SERVICES_AUDIO_MANAGER_H_