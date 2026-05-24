#include "services/audio_manager.h"

// algorithm 用于计算 peak 时取最大值。
#include <algorithm>
// cmath 提供 sqrt，用于 RMS 计算。
#include <cmath>
// cstdint 提供固定宽度 PCM/原始字节类型。
#include <cstdint>
// cstring 提供 memcpy，用于无别名风险地还原 float32。
#include <cstring>
// memory 提供 PImpl 的 unique_ptr/make_unique。
#include <memory>
// stdexcept/string 用于向上抛出带上下文的 PortAudio 错误。
#include <stdexcept>
#include <string>
// vector 承载输入样本、输出样本和原始 PCM bytes。
#include <vector>

// PortAudio C API 是底层音频设备访问入口。
#include <portaudio.h>

// 配置快照提供 audio_input/audio_output 参数。
#include "common/config.h"
// 所有诊断和错误都通过项目 logger 输出。
#include "common/logger.h"

namespace interview::services {
namespace {

// 实时接口固定使用单声道，避免设备配置影响协议格式。
constexpr int kMonoChannels = 1;
// 输入是 16-bit PCM；输出配置也保持同一 bit_size 字段语义。
constexpr int kPcmBitSize = 16;
// 项目配置中使用 "pcm" 表示原始 PCM 数据。
constexpr const char* kPcmFormat = "pcm";
// 麦克风输入默认 16 kHz，匹配 ASR 上行要求。
constexpr int kDefaultInputSampleRate = 16000;
// 3200 frames 约等于 200ms 16 kHz mono 音频。
constexpr int kDefaultInputChunk = 3200;
// TTS 下行默认 24 kHz，匹配当前服务端 float32 PCM 输出。
constexpr int kDefaultOutputSampleRate = 24000;
// 4800 frames 约等于 200ms 24 kHz mono 音频。
constexpr int kDefaultOutputChunk = 4800;

int PositiveOrDefault(int value, int fallback) {
    // 配置值必须为正；无效值回退到代码默认值。
    return value > 0 ? value : fallback;
}

std::string PaErrorText(PaError error) {
    // PortAudio 返回 const char*，这里转成 std::string 便于拼接异常信息。
    return Pa_GetErrorText(error);
}

void NormalizeInputConfig(interview::common::AudioConfig& config) {
    // 输入协议固定为 mono/16-bit/pcm，不接受配置改成其他格式。
    if (config.channels != kMonoChannels || config.bit_size != kPcmBitSize ||
        config.format != kPcmFormat) {
        LOG_WARN("audio_input must be mono 16-bit pcm; forcing fixed format");
    }
    // 强制写回固定协议字段。
    config.channels = kMonoChannels;
    config.bit_size = kPcmBitSize;
    config.format = kPcmFormat;
    // sample_rate/chunk 允许配置，但必须是正数。
    config.sample_rate =
        PositiveOrDefault(config.sample_rate, kDefaultInputSampleRate);
    config.chunk = PositiveOrDefault(config.chunk, kDefaultInputChunk);
}

void NormalizeOutputConfig(interview::common::AudioConfig& config) {
    // 输出协议固定为 mono/pcm，实际 PortAudio sampleFormat 在 OpenOutputStream 里指定为 float32。
    if (config.channels != kMonoChannels || config.format != kPcmFormat) {
        LOG_WARN("audio_output must be mono pcm; forcing fixed format");
    }
    // 强制写回项目统一的输出配置。
    config.channels = kMonoChannels;
    config.bit_size = kPcmBitSize;
    config.format = kPcmFormat;
    // 输出采样率和 chunk 同样接受正数配置。
    config.sample_rate =
        PositiveOrDefault(config.sample_rate, kDefaultOutputSampleRate);
    config.chunk = PositiveOrDefault(config.chunk, kDefaultOutputChunk);
    // 过小的阻塞写入块容易造成 TTS underflow，这里提高到稳定下限。
    if (config.chunk < kDefaultOutputChunk) {
        LOG_INFO("audio_output.chunk={} is too small for stable blocking "
                 "TTS playback; using {} frames",
                 config.chunk, kDefaultOutputChunk);
        config.chunk = kDefaultOutputChunk;
    }
}

void ThrowIfPaError(PaError err, const std::string& operation) {
    // 统一把 PortAudio 错误码转换成带操作上下文的 C++ 异常。
    if (err != paNoError) {
        throw std::runtime_error(operation + ": " + PaErrorText(err));
    }
}

}  // namespace

class AudioManager::Impl {
public:
    Impl() {
        // 构造时读取一次配置快照，避免运行中配置变更影响已打开的流。
        const auto snapshot = interview::common::Config::Instance().Snapshot();
        input_config_ = snapshot.audio_input;
        output_config_ = snapshot.audio_output;
        // 将配置规整到实时协议支持的固定格式。
        NormalizeInputConfig(input_config_);
        NormalizeOutputConfig(output_config_);

        // PortAudio 进程级初始化；失败后对象保持未初始化状态。
        const PaError err = Pa_Initialize();
        if (err != paNoError) {
            LOG_ERROR("PortAudio initialize failed: {}", PaErrorText(err));
            initialized_ = false;
            return;
        }
        initialized_ = true;
        // 初始化成功后输出设备诊断，方便定位默认设备和 host api。
        LogPortAudioDevices();
    }

    ~Impl() {
        // 析构统一走 Cleanup，确保输入/输出流和 PortAudio 都被释放。
        Cleanup();
    }

    void OpenInputStream() {
        // 未初始化成功时不允许继续打开设备。
        if (!initialized_) {
            throw std::runtime_error("PortAudio is not initialized");
        }
        // 重复打开保持幂等。
        if (input_stream_ != nullptr) {
            return;
        }

        // PortAudio 输入参数从默认输入设备开始构造。
        PaStreamParameters params{};
        params.device = Pa_GetDefaultInputDevice();
        if (params.device == paNoDevice) {
            throw std::runtime_error("no default input audio device");
        }

        // 设备信息提供默认低延迟参数。
        const PaDeviceInfo* device_info = Pa_GetDeviceInfo(params.device);
        if (device_info == nullptr) {
            throw std::runtime_error("failed to query default input audio device");
        }

        // 输入流格式固定为 int16 mono。
        params.channelCount = input_config_.channels;
        params.sampleFormat = paInt16;
        params.suggestedLatency = device_info->defaultLowInputLatency;
        params.hostApiSpecificStreamInfo = nullptr;

        // 打开阻塞式输入流，不使用 callback。
        PaError err = Pa_OpenStream(&input_stream_,
                                    &params,
                                    nullptr,
                                    input_config_.sample_rate,
                                    input_config_.chunk,
                                    paClipOff,
                                    nullptr,
                                    nullptr);
        ThrowIfPaError(err, "open input stream failed");

        try {
            // 打开成功后立即启动流，后续 ReadAudio 才能阻塞读取。
            err = Pa_StartStream(input_stream_);
            ThrowIfPaError(err, "start input stream failed");
        } catch (...) {
            // 启动失败时关闭刚打开的流，保持对象状态干净。
            Pa_CloseStream(input_stream_);
            input_stream_ = nullptr;
            throw;
        }

        // 复用输入缓冲，避免每次读取都重新分配底层数组。
        input_samples_.assign(
            static_cast<std::size_t>(input_config_.chunk * input_config_.channels),
            0);
        LOG_INFO("audio input started: {} Hz, chunk={} frames",
                 input_config_.sample_rate, input_config_.chunk);
    }

    std::vector<int16_t> ReadAudio() {
        // 输入流未打开时返回空样本，调用方可继续轮询。
        if (input_stream_ == nullptr) {
            return {};
        }

        // 阻塞读取一个 chunk 的 int16 样本。
        const PaError err =
            Pa_ReadStream(input_stream_, input_samples_.data(),
                          input_config_.chunk);
        if (err == paInputOverflowed) {
            // overflow 时仍返回缓冲中的最新数据，尽量不中断语音链路。
            LOG_WARN("input stream overflowed; sending latest available chunk");
        } else if (err != paNoError) {
            // 其他错误返回空数组，由录音循环短暂休眠后重试。
            LOG_WARN("read input stream failed: {}", PaErrorText(err));
            return {};
        }
        // 返回一份当前 chunk 样本，调用方会转换为 pcm_s16le bytes。
        return input_samples_;
    }

    void StopInput() {
        // 未打开输入流时保持幂等。
        if (input_stream_ == nullptr) {
            return;
        }

        // 先停流，再关闭流；paStreamIsStopped 说明已经停过，可忽略。
        PaError err = Pa_StopStream(input_stream_);
        if (err != paNoError && err != paStreamIsStopped) {
            LOG_WARN("failed to stop input stream: {}", PaErrorText(err));
        }

        // 关闭底层输入流句柄。
        err = Pa_CloseStream(input_stream_);
        if (err != paNoError) {
            LOG_WARN("failed to close input stream: {}", PaErrorText(err));
        }

        // 清空状态，允许未来重新 OpenInputStream。
        input_stream_ = nullptr;
        input_samples_.clear();
        LOG_INFO("audio input stopped");
    }

    void OpenOutputStream() {
        // 未初始化成功时不允许继续打开输出设备。
        if (!initialized_) {
            throw std::runtime_error("PortAudio is not initialized");
        }
        // 重复打开保持幂等。
        if (output_stream_ != nullptr) {
            return;
        }

        // PortAudio 输出参数从默认输出设备开始构造。
        PaStreamParameters params{};
        params.device = Pa_GetDefaultOutputDevice();
        if (params.device == paNoDevice) {
            throw std::runtime_error("no default output audio device");
        }

        // 输出设备信息提供默认高延迟参数，适合阻塞式 TTS 播放稳定性。
        const PaDeviceInfo* device_info = Pa_GetDeviceInfo(params.device);
        if (device_info == nullptr) {
            throw std::runtime_error("failed to query default output audio device");
        }

        // 输出流写入 float32 mono 样本。
        params.channelCount = output_config_.channels;
        params.sampleFormat = paFloat32;
        params.suggestedLatency = device_info->defaultHighOutputLatency;
        params.hostApiSpecificStreamInfo = nullptr;

        // 打开阻塞式输出流，不使用 callback。
        PaError err = Pa_OpenStream(&output_stream_,
                                    nullptr,
                                    &params,
                                    output_config_.sample_rate,
                                    output_config_.chunk,
                                    paClipOff,
                                    nullptr,
                                    nullptr);
        ThrowIfPaError(err, "open output stream failed");

        try {
            // 打开成功后立即启动流，后续 WriteAudio 才能写入。
            err = Pa_StartStream(output_stream_);
            ThrowIfPaError(err, "start output stream failed");
        } catch (...) {
            // 启动失败时关闭刚打开的输出流，保持对象可重试。
            Pa_CloseStream(output_stream_);
            output_stream_ = nullptr;
            throw;
        }

        LOG_INFO("audio output started: {} Hz, chunk={} frames",
                 output_config_.sample_rate, output_config_.chunk);
    }

    bool OpenStreams() {
        try {
            OpenInputStream();
            OpenOutputStream();
            return true;
        } catch (const std::exception& e) {
            LOG_ERROR("audio device startup failed; voice I/O disabled: {}",
                      e.what());
            Cleanup();
            return false;
        }
    }

    void WriteAudio(const std::vector<float>& samples) {
        // 调用方必须先打开输出流。
        if (output_stream_ == nullptr) {
            throw std::runtime_error("output audio stream is not open");
        }
        // 空样本没有可播放内容。
        if (samples.empty()) {
            return;
        }

        // PortAudio 按 frame 计数；mono 下 frame 数等于样本数。
        const auto frames =
            static_cast<unsigned long>(samples.size() / output_config_.channels);
        if (frames == 0) {
            return;
        }

        // 阻塞写入，返回后该批样本已交给本地输出流。
        const PaError err =
            Pa_WriteStream(output_stream_, samples.data(), frames);
        if (err == paOutputUnderflowed) {
            // underflow 说明播放端暂时饿包，记录但不中断整场会话。
            LOG_WARN("output stream underflowed");
        } else if (err != paNoError) {
            // 其他错误交给上层播放循环记录并决定是否继续。
            throw std::runtime_error("write output stream failed: " +
                                     PaErrorText(err));
        }
    }

    void StopOutput() {
        // 未打开输出流时保持幂等。
        if (output_stream_ == nullptr) {
            return;
        }

        // 先停输出流，再关闭句柄。
        PaError err = Pa_StopStream(output_stream_);
        if (err != paNoError && err != paStreamIsStopped) {
            LOG_WARN("failed to stop output stream: {}", PaErrorText(err));
        }

        // 关闭底层输出流句柄。
        err = Pa_CloseStream(output_stream_);
        if (err != paNoError) {
            LOG_WARN("failed to close output stream: {}", PaErrorText(err));
        }

        // 清空状态，允许未来重新 OpenOutputStream。
        output_stream_ = nullptr;
        LOG_INFO("audio output stopped");
    }

    void Cleanup() {
        // 输入/输出停止函数都是幂等的，可以无条件调用。
        StopInput();
        StopOutput();
        // 如果初始化失败过，则没有可终止的 PortAudio runtime。
        if (!initialized_) {
            return;
        }

        // 终止 PortAudio 进程级资源。
        const PaError err = Pa_Terminate();
        if (err != paNoError) {
            LOG_WARN("PortAudio terminate returned: {}", PaErrorText(err));
        }
        // 标记已终止，避免析构时重复 Terminate。
        initialized_ = false;
    }

private:
    void LogPortAudioDevices() const {
        // 读取 host api、设备数量和默认输入/输出索引。
        const int host_api_count = Pa_GetHostApiCount();
        const int device_count = Pa_GetDeviceCount();
        const PaDeviceIndex default_in = Pa_GetDefaultInputDevice();
        const PaDeviceIndex default_out = Pa_GetDefaultOutputDevice();
        LOG_INFO("PortAudio diag: host_api_count={}, device_count={}, "
                 "default_input={}, default_output={}",
                 host_api_count, device_count, static_cast<int>(default_in),
                 static_cast<int>(default_out));

        // 逐个打印 host api，帮助判断当前走 ALSA、PulseAudio、WASAPI 等哪条路径。
        for (int i = 0; i < host_api_count; ++i) {
            const PaHostApiInfo* api = Pa_GetHostApiInfo(i);
            if (api == nullptr) {
                continue;
            }
            LOG_INFO("PortAudio diag: host_api[{}] name='{}' type={} "
                     "device_count={} default_in={} default_out={}",
                     i, api->name, static_cast<int>(api->type),
                     api->deviceCount,
                     static_cast<int>(api->defaultInputDevice),
                     static_cast<int>(api->defaultOutputDevice));
        }

        // 逐个打印设备能力，便于定位默认设备是否有输入/输出通道。
        for (int i = 0; i < device_count; ++i) {
            const PaDeviceInfo* dev = Pa_GetDeviceInfo(i);
            if (dev == nullptr) {
                continue;
            }
            LOG_INFO("PortAudio diag: device[{}] name='{}' host_api={} "
                     "max_in_ch={} max_out_ch={} default_sr={}",
                     i, dev->name, dev->hostApi, dev->maxInputChannels,
                     dev->maxOutputChannels, dev->defaultSampleRate);
        }
    }

    // 规整后的输入配置，仅在构造时写入。
    interview::common::AudioConfig input_config_;
    // 规整后的输出配置，仅在构造时写入。
    interview::common::AudioConfig output_config_;
    // PortAudio 输入流句柄，OpenInputStream/StopInput 管理生命周期。
    PaStream* input_stream_ = nullptr;
    // PortAudio 输出流句柄，OpenOutputStream/StopOutput 管理生命周期。
    PaStream* output_stream_ = nullptr;
    // 复用的输入读取缓冲区。
    std::vector<int16_t> input_samples_;
    // 标记 Pa_Initialize 是否成功，决定 Cleanup 是否调用 Pa_Terminate。
    bool initialized_ = false;
};

// 构造 AudioManager 时创建真正持有 PortAudio 资源的 Impl。
AudioManager::AudioManager() : impl_(std::make_unique<Impl>()) {}

// Impl 析构会自动 Cleanup，因此外层析构保持默认即可。
AudioManager::~AudioManager() = default;

void AudioManager::OpenInputStream() {
    // 转发给 Impl，保持头文件不暴露 PortAudio 类型。
    impl_->OpenInputStream();
}

bool AudioManager::OpenStreams() {
    // 在 AudioManager 边界把设备打开异常转换为 bool 结果。
    return impl_->OpenStreams();
}

std::vector<int16_t> AudioManager::ReadAudio() {
    // 返回一块 int16 麦克风样本。
    return impl_->ReadAudio();
}

void AudioManager::StopInput() {
    // 停止并关闭输入流。
    impl_->StopInput();
}

void AudioManager::OpenOutputStream() {
    // 打开默认输出设备。
    impl_->OpenOutputStream();
}

void AudioManager::WriteAudio(const std::vector<float>& samples) {
    // 写入一段 float32 TTS 样本。
    impl_->WriteAudio(samples);
}

void AudioManager::StopOutput() {
    // 停止并关闭输出流。
    impl_->StopOutput();
}

void AudioManager::Cleanup() {
    // 显式释放全部 PortAudio 资源。
    impl_->Cleanup();
}

std::vector<float> AudioManager::Float32PcmLeToSamples(
    const std::vector<uint8_t>& pcm) {
    // 每 4 个字节还原一个 float32 样本。
    std::vector<float> out;
    out.reserve(pcm.size() / sizeof(float));

    for (std::size_t i = 0; i + sizeof(float) <= pcm.size(); i += sizeof(float)) {
        // 按小端顺序组装 IEEE-754 float 的原始 32-bit 位模式。
        uint32_t raw = static_cast<uint32_t>(pcm[i]) |
                       (static_cast<uint32_t>(pcm[i + 1]) << 8) |
                       (static_cast<uint32_t>(pcm[i + 2]) << 16) |
                       (static_cast<uint32_t>(pcm[i + 3]) << 24);
        // 用 memcpy 避免 strict-aliasing 问题。
        float sample = 0.0F;
        static_assert(sizeof(sample) == sizeof(raw));
        std::memcpy(&sample, &raw, sizeof(sample));
        // 保存还原后的样本，供 PortAudio paFloat32 输出流写入。
        out.push_back(sample);
    }
    return out;
}

Pcm16Level AudioManager::Pcm16LeLevel(const std::vector<uint8_t>& pcm) {
    // 默认 peak/rms 都为 0，空输入直接返回该值。
    Pcm16Level level;
    // sum_squares 用于最后计算 RMS。
    double sum_squares = 0.0;
    // count 记录有效 int16 样本数。
    std::size_t count = 0;

    // 每两个字节按小端解析一个 int16 样本。
    for (std::size_t i = 0; i + 1 < pcm.size(); i += 2) {
        int value = static_cast<int>(pcm[i]) |
                    (static_cast<int>(pcm[i + 1]) << 8);
        // 手动做二补码符号扩展，得到有符号 int16 数值。
        if (value >= 0x8000) {
            value -= 0x10000;
        }

        // -32768 的绝对值超出 int16 正数范围，特殊处理为 32768。
        const int abs_value = value == -32768 ? 32768 : std::abs(value);
        // peak 保存当前段内最大的绝对振幅。
        level.peak = std::max(level.peak, abs_value);
        // RMS 使用原始有符号样本平方和。
        sum_squares += static_cast<double>(value) * value;
        ++count;
    }

    // 有有效样本时才计算均方根，避免除以 0。
    if (count > 0) {
        level.rms = std::sqrt(sum_squares / static_cast<double>(count));
    }
    // 返回可直接用于日志输出的音量统计。
    return level;
}

}  // namespace interview::services
