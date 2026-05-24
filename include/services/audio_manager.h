#ifndef INCLUDE_SERVICES_AUDIO_MANAGER_H_
#define INCLUDE_SERVICES_AUDIO_MANAGER_H_

#include <cstdint>
#include <memory>
#include <vector>

namespace interview::services {

// Pcm16Level 保存一段 16-bit PCM 的音量统计结果。
struct Pcm16Level {
    // peak 是绝对值峰值，用来快速判断输入是否接近静音或爆音。
    int peak = 0;
    // rms 是均方根音量，比 peak 更适合观察持续音量大小。
    double rms = 0.0;
};

// Owns the PortAudio input/output streams used by the realtime voice path.
//
// AudioManager is intentionally small:
// - input: 16 kHz mono int16 PCM, returned as samples;
// - output: 24 kHz mono float32 PCM, written as samples;
// - no worker threads, queues, or session state live here.
class AudioManager {
public:
    // 构造时创建 PortAudio 实现对象并读取当前音频配置快照。
    AudioManager();
    // 析构时释放输入/输出流并终止 PortAudio。
    ~AudioManager();

    // PortAudio 流句柄不可复制，避免两个对象同时关闭同一个底层流。
    AudioManager(const AudioManager&) = delete;
    AudioManager& operator=(const AudioManager&) = delete;

    // 这里也禁用移动，保证 Impl 的生命周期始终由一个 AudioManager 独占。
    AudioManager(AudioManager&&) = delete;
    AudioManager& operator=(AudioManager&&) = delete;

    // 打开默认输入设备，格式固定为 16 kHz/mono/int16 PCM。
    void OpenInputStream();
    // 打开输入和输出流；失败时清理已打开的流并返回 false。
    bool OpenStreams();
    // 阻塞读取一块麦克风样本；失败或未打开时返回空数组。
    std::vector<int16_t> ReadAudio();
    // 停止并关闭输入流。
    void StopInput();

    // 打开默认输出设备，格式固定为 24 kHz/mono/float32 PCM。
    void OpenOutputStream();
    // 阻塞写入一段 float32 TTS 样本到扬声器。
    void WriteAudio(const std::vector<float>& samples);
    // 停止并关闭输出流。
    void StopOutput();

    // 显式释放所有 PortAudio 资源；析构函数也会调用同一条清理路径。
    void Cleanup();

    // 将服务端下发的小端 float32 PCM bytes 转成 PortAudio 可写的样本数组。
    static std::vector<float> Float32PcmLeToSamples(
        const std::vector<uint8_t>& pcm);
    // 计算小端 int16 PCM bytes 的 peak/rms，主要用于音频调试日志。
    static Pcm16Level Pcm16LeLevel(const std::vector<uint8_t>& pcm);

private:
    // Impl 隔离 portaudio.h 细节，减少头文件对外暴露的第三方依赖。
    class Impl;
    // PImpl 独占底层设备、流句柄和缓冲区。
    std::unique_ptr<Impl> impl_;
};

} // namespace interview::services

#endif // INCLUDE_SERVICES_AUDIO_MANAGER_H_
