#include "services/audio_manager.h"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <portaudio.h>

#include "common/config.h"
#include "common/logger.h"


namespace interview::services {
namespace {

// Stage 8 阶段的实时对话音频协议是固定的。
//
// 虽然代码仍然会从配置文件中读取 chunk 大小、采样率等默认值，
// 但真正和服务端通信、以及本地播放时使用的音频格式是固定的：
//
// - 单声道 mono；
// - 16-bit PCM；
// - 原始 PCM 字节流格式。
//
// 这样做的目的是保证：
// 1. 客户端发送给服务端的音频格式稳定；
// 2. 服务端返回的音频格式与 PortAudio 播放逻辑一致；
// 3. 避免配置文件被误改后导致协议不匹配。
constexpr int kMonoChannels = 1;

// PCM 位深 16 bit
// 每个采样点占用两个字节
constexpr int kPcmBitSize = 16;

// 音频格式固定位 "pcm"
// 这里的pcm 指的是未压缩的原始 PCM 音频数据
// 不是 wav、mp3、aac等带封装或压缩格式
constexpr const char* kPcmFormat = "pcm";

// 判断音频是否满足要求
// - 单声道;
// - 16-bit;
// - pcm 格式。

// 参数：
// -config: 从配置系统读取出来的音频配置>
// 返回值：
// - true: 配置已经是mono 16-bit PCM;
// - false: 配置不符合要求，后续需要强制修正。
bool IsPcm16Mono(const interview::common::AudioConfig& config) {
    return config.channels == kMonoChannels &&
           config.bit_size == kPcmBitSize &&
           config.format == kPcmFormat;
}

// 如果value是正数，就返回value；否则返回fallback
int PostiveOrDefault(int value, int fallback) {
    return value > 0 ? value : fallback;
}

// PortAudio 的错误码转成人类可读的错误字符串
// Pa_GetErrorText() 是 PortAudio 提供的错误解释函数。
std::string PaErrorText(PaError error) {
    return Pa_GetErrorText(error);
}

} // namespace

class AudioManager::Impl {
public:
    Impl() {
        const auto snapshot = interview::common::Config::Instance().Snapshot();

        input_config_ = snapshot.audio_input;
        output_config_ = snapshot.audio_output;

        // PortAudio 边界处统一音频格式
        if (!IsPcm16Mono(input_config_)) {
            LOG_WARN("用户输入必须为单声道 16 bit的pcm；强制修复为固定格式");
             
            // 输入音频强制使用单声道
            input_config_.channels = kMonoChannels;

            // 输入音频强制使用 16-bit PCM
            input_config_.bit_size = kPcmBitSize;

            // 输入音频强制标记为pcm格式
            input_config_.format = kPcmFormat;
        }
        
        // 输出音频 同样必须符合 mono 16-bit PCM
        if (!IsPcm16Mono(output_config_)) {
            LOG_WARN("用户输出必须为单声道 16 bit的pcm；强制修复为固定格式");

            output_config_.channels = kMonoChannels;
            output_config_.bit_size = kPcmBitSize;
            output_config_.format = kPcmFormat;
        }
        // 设置输入采样率
        // 16kHz 语音识别 / 实时语音输入中常见的采样率
        input_config_.sample_rate = 
            PostiveOrDefault(input_config_.sample_rate, 16000);
        // 设置输入 chunk 大小
        // 默认值 160 在 16kHz 下约等于 10ms音频
        input_config_.chunk = PostiveOrDefault(input_config_.chunk, 160);

        // 设置输出采样率
        output_config_.sample_rate = 
            PostiveOrDefault(output_config_.sample_rate, 24000);

        // 设置输出 chunk 大小
        // 默认值240 在 24kHz 下约等于 10ms音频
        output_config_.chunk = PostiveOrDefault(output_config_.chunk, 240);

        // 初始化 PortAudio
        const PaError err = Pa_Initialize();
        if (err != paNoError) {
            LOG_ERROR("PortAudio initialize failed: {}", PaErrorText(err));
            initialized_ = false;
            return;
        }

        initialized_ = true;
    }
    ~Impl() {
        StopInput();

        StopOutput();
    }
    // 启动音频输入
    //
    bool StartInput() {
        if (!initialized_) {
            return false;
        }
        if (input_stream_ != nullptr) {
            return true;
        }
        // 输入流参数
        // PaStreamParameters 用于告诉 PortAudio: 
        // - 使用哪个设备；
        // - 通道数是多少；
        // - 采样格式是什么；
        // - 建议延迟是多少；
        // - 是否有平台相关的扩展配置
        PaStreamParameters input_params{};

        input_params.device = Pa_GetDefaultInputDevice();

        if (input_params.device == paNoDevice) {
            LOG_ERROR("没有默认的输入设备");
            return false;
        }
        // 查询默认输入设备的信息
        const PaDeviceInfo* device_info = Pa_GetDeviceInfo(input_params.device);
        if (device_info == nullptr) {
            LOG_ERROR("failed to query default input audio device");
            return false;
        }

        // 设置输入通道数, 为1，也就是单声道
        input_params.channelCount = input_config_.channels;
        // 设置输入采样格式为 paInt16
        input_params.sampleFormat = paInt16;
        // 使用设备推荐的低输入延迟
        input_params.suggestedLatency = device_info->defaultLowInputLatency;
        // 不使用平台相关的额外流信息
        input_params.hostApiSpecificStreamInfo = nullptr;

        // 打开输入流
        PaError err = Pa_OpenStream(&input_stream_,
                                    &input_params,
                                    nullptr,
                                    input_config_.sample_rate,
                                    input_config_.chunk,
                                    paNoFlag,
                                    nullptr,
                                    nullptr);
        if (err != paNoError) {
            LOG_ERROR("open input stream failed :{}", PaErrorText(err));
            input_stream_ = nullptr;
            return false;
        }

        // 启动输入流
        err = Pa_StartStream(input_stream_);
        if (err != paNoError) {
            LOG_ERROR("start input stream failed : {}", PaErrorText(err));

            // 如果启动失败，需要关闭刚刚打开的流，避免资源泄露
            Pa_CloseStream(input_stream_);
            input_stream_ = nullptr;
            return false;
        }
        // 为输入采样缓存分配空间
        // 缓存大小 = chunk 帧数 * 通道数
        input_samples_.assign(static_cast<std::size_t>(input_config_.chunk * input_config_.channels), 0);

        LOG_INFO("audio input started : {} Hz, chunk = {} frames", input_config_.sample_rate, input_config_.chunk);
        return true;
    }
    // 读取一个输入音频块
    bool ReadInputChunk(std::vector<uint8_t>& pcm) {
        // 输入流没有启动，无法读取
        if (input_stream_ == nullptr) {
            return false;
        }
        // 从PortAudio 输入流阻塞读取一个 chunk。
        // input_samples_.data() 是int16_t 缓冲区。
        // ipput_config_.chunk 表示读取多少帧
        const PaError err = Pa_ReadStream(input_stream_, input_samples_.data(),
                                            input_config_.chunk);
        // 如果发生普通读取错误，返回false
        // paInputOverflowed 是特殊情况：
        // 表示输入设备的数据来得太快，中间可能有数据溢出。
        // 但当前缓冲区里仍然可能有可用的最新音频，
        // 所以这里不把它当作致命失败。
        if (err != paNoError && err != paInputOverflowed) {
            LOG_WARN("read input stream failed : {}", PaErrorText(err));
            return false;
        }
        if (err == paInputOverflowed) {
            LOG_DEBUG("input stream overflowed; sending latest available chunk");
        }

        pcm.clear();
        // 预留足够空间。
        // 每个 int16_t 采样点占 2 个字节
        pcm.reserve(input_samples_.size() * sizeof(int16_t));

        // 将 PortAudio 返回的 int16_t 样本序列化为小端序字节
        for (const int16_t sample : input_samples_) {
            // PortAudio 返回的是主机字节序的 int16。
            // 但是实时协议要求 little-endian 小端序字节，
            // 因此这里不能直接把内存 reinterpret_cast 成 uint8_t 数组发送。
            // 显式拆成低字节、高字节，可以保证在大端 / 小端机器上都得到一致结果。
            const auto u = static_cast<uint16_t>(sample);
            // 低8为先写入
            pcm.push_back(static_cast<uint8_t>(u & 0xFF));
            // 高8位后写入
            pcm.push_back(static_cast<uint8_t>((u >> 8) & 0xFF));
        }
        return true;
    }
    // 停止音频输入
    // 如果输入流已经打开，则停止并关闭它
    // 如果输入流没有打开，则直接返回
    void StopInput() {
        if (input_stream_ == nullptr) {
            return;
        }
        // 停止输入流
        Pa_StopStream(input_stream_);
        // 关闭输入流并释放 PortAudio 流资源
        Pa_CloseStream(input_stream_);

        // 防止悬空指针
        input_stream_ = nullptr;
        // 清空输入采样缓存
        input_samples_.clear();
        LOG_INFO("audio input stopped");
    }
    // 启动音频输出。
    //
    // 这个函数会打开系统默认输出设备，通常是默认扬声器或耳机。
    // 成功后，可以调用 PlayTtsPcm() 播放服务端返回的 TTS PCM 数据。
    //
    // 返回值：
    // - true：输出流已经启动或本来就已经启动；
    // - false：启动失败。
    bool StartOutput() {
        // PortAudio 没有初始化成功时，不能打开输出流
        if (!initialized_) {
            return false;
        }
        // 如果输出流已经打开，直接返回成功
        if (output_stream_ != nullptr) {
            return true;
        }
        // 输出流参数
        PaStreamParameters output_params{};

        // 获取系统默认输出设备
        output_params.device = Pa_GetDefaultOutputDevice();
        // 如果没有默认输出设备，直接失败
        if (output_params.device == paNoDevice) {
            LOG_ERROR("no default output audio deive");
            return false;
        }

        // 查询默认输出设备信息
        // 这里主要用于获取默认低延迟输出参数
        const PaDeviceInfo* device_info = Pa_GetDeviceInfo(output_params.device);
        if (device_info == nullptr) {
            LOG_ERROR("failed to query default output audio device");
            return false;
        }

        output_params.channelCount = output_config_.channels;
        // 输出流使用 float32 格式。
        output_params.sampleFormat = paFloat32;
        // 使用设备推荐的低输出延迟。
        output_params.suggestedLatency = device_info->defaultLowOutputLatency;
        output_params.hostApiSpecificStreamInfo = nullptr;

        // 打开输出流
        PaError err = Pa_OpenStream(&output_stream_,
                                    nullptr,
                                    &output_params,
                                    output_config_.sample_rate,
                                    output_config_.chunk,
                                    paNoFlag,
                                    nullptr,
                                    nullptr);
        if (err != paNoError) {
            LOG_ERROR("open output stream failed : {}", PaErrorText(err));
            output_stream_ = nullptr;
            return false;
        }
        // 启动输出流
        err = Pa_StartStream(output_stream_);
        if (err != paNoError) {
            LOG_ERROR("start output stream failed : {}", PaErrorText(err));
            // 如果启动失败，需要关闭已经打开的流，避免资源泄露
            Pa_CloseStream(output_stream_);
            return false;
        }

        LOG_INFO("audio output started: {} Hz, chunk={} frames", 
                    output_config_.sample_rate, output_config_.chunk);
        return true;
    }
    // 播放一段服务端返回的 TTS PCM 音频
    // pcm ：服务端返回的 int16 little-endian PCM 字节流
    bool PlayTtsPcm(const std::vector<uint8_t>& pcm) {
        // 输出流没有启动，或者传入数据为空时，不进行播放
        if (output_stream_ == nullptr || pcm.empty()) {
            return false;
        }
        // 将 int16 little-endian PCM 转换成 float32.
        // PortAudio 输出流使用 paFloat32,
        std::vector<float> samples = AudioManager::Pcm16LeToFloat32(pcm);

        // 如果转换结果为空，说明输入没有完整的 int16 采样点
        // 例如 pcm 只有 1 个字节，无法组成一个 int16。
        // 这里返回 true，表示没有真正的播放错误，只是没有可播放数据。
        if (samples.empty()) {
            return true;
        }

        // 计算需要写入的帧数
        const auto frames = static_cast<unsigned long>(samples.size() /  output_config_.channels);

        // 阻塞式写入 PortAudio 输出流。
        // samples.data() 是float32 样本数组
        // frames 是要写入的音频帧数
        const PaError err = Pa_WriteStream(output_stream_, samples.data(), frames);

        // paOutputUnderflowed 表示输出下溢，
        // 也就是播放端没有及时拿到足够数据，可能造成轻微卡顿。
        //
        // 这里不把它当作致命错误，避免播放线程因为一次下溢而停止。
        if (err != paNoError && err != paOutputUnderflowed) {
            LOG_WARN("write output stream failed : {}", PaErrorText(err));
            return false;
        }
        return true;
    }

    // 停止音频输出。
    // 如果输出流已经打开，则停止并关闭它。
    // 如果输出流没有打开，则直接返回。
    void StopOutput() {
        // 输出流为空，说明没有可关闭的输出资源
        if (output_stream_ == nullptr) {
            return;
        }
        // 停止输出流
        Pa_StopStream(output_stream_);
        // 关闭输出流并释放 PortAudio 流资源
        Pa_CloseStream(output_stream_);
        output_stream_ = nullptr;
        LOG_INFO("audio output stopped");
    }
private:
    // 输入音频配置。
    //
    // 主要包含：
    // - sample_rate：采样率，例如 16000；
    // - chunk：每次读取的帧数，例如 160；
    // - channels：通道数，Stage 8 强制为 1；
    // - bit_size：位深，Stage 8 强制为 16；
    // - format：格式，Stage 8 强制为 pcm。
    interview::common::AudioConfig input_config_;
    // 输出音频配置
    // 主要包含：
    // - sampel_rate：采样率， 例如 24000；
    // - chunk: 每次写入的帧数建议，例如240；
    // - channels: 通道数，Stage 8 强制为1；
    // - bit_size: 位深，Stage 8 强制为16；
    // - format: 格式，Stage 8 强制为pcm。
    interview::common::AudioConfig output_config_;

    // PortAudio 输入流指针。
    // nullptr 表示输入流未打开
    // 非 nullptr 表示输入流已经由 Pa_OpenStream() 创建。
    PaStream* input_stream_ = nullptr;
    // PortAudio 输出流指针。
    // nullptr 表示输出流未打开
    // 非 nullptr 表示输入流已经由 Pa_OpenStream() 创建。
    PaStream* output_stream_ = nullptr;

    // 输入采样缓存。
    // Pa_ReadStream() 会把 int16 音频样本写到这个 vector中。
    // 随后 ReadInputChunk() 会把这些 int16 样本显示序列化成小端序字节。
    std::vector<int16_t> input_samples_;
    // PortAudio 是否初始化成功
    // StartInput() 和StartOutput() 都会检查这个标志
    bool initialized_ = false;
};


// AudioManager 构造函数

AudioManager::AudioManager() : impl_(std::make_unique<Impl>()) {
    
}

AudioManager::~AudioManager() = default;


bool AudioManager::StartInput() {
    return impl_->StartInput();
}

bool AudioManager::ReadInputChunk(std::vector<uint8_t>& pcm) {
    return impl_->ReadInputChunk(pcm);
}

void AudioManager::StopInput() {
    return impl_->StopInput();
}

bool AudioManager::StartOutput() {
    return impl_->StartOutput();
}

bool AudioManager::PlayTtsPcm(const std::vector<uint8_t>& pcm) {
    return impl_->PlayTtsPcm(pcm);
}

void AudioManager::StopOutput() {
    return impl_->StopOutput();
}

// 将 little-endian int16 PCM 字节流转换为 float32 样本。
//
// 输入：
// - pcm：原始 PCM 字节数组；
// - 每两个字节表示一个 int16 采样点；
// - 字节序为 little-endian，也就是低字节在前，高字节在后。
//
// 输出：
// - std::vector<float>；
// - 每个采样点归一化到 [-1.0, 1.0) 范围。
//
// 典型转换关系：
// - 0x00 0x00 -> 0 -> 0.0；
// - 0xFF 0x7F -> 32767 -> 约 0.99997；
// - 0x00 0x80 -> -32768 -> -1.0。
std::vector<float> AudioManager::Pcm16LeToFloat32(const std::vector<uint8_t>& pcm) {
    std::vector<float> out;

    // 每两个字节组成一个int16_t
    out.reserve(pcm.size() / sizeof(int16_t));

    // 每次处理两个字节
    // 条件 i + 1 < pcm.size() 可以保证存在完整的两个字节。
    // 如果pcm 最后只剩一个不完整的字节，会被自然忽略。
    for (std::size_t i = 0; i + 1 < pcm.size(); i += 2) {
        // little-endian 的低字节
        const int low = pcm[i];
        // little-endian 的高字节，左移8位
        const int high = pcm[i + 1] << 8;
        int value = low | high;
        // 如果最高位为 1， 说明这是一个负数的二进制补码表示
        // 例如：
        // - 0x8000 应解释为 -32768
        // - 0xFFFF 应解释为 -1
        // 通过减去 0x10000, 把无符号范围 32768..65535
        // 映射到有符号范围 -32768..-1
        if (value >= 0x8000) {
            value -= 0x10000;
        }
        // 将 int16 范围归一化到float
        // 除以 32768.0F 后
        // - -32768 -> -1.0;
        // - 0 -> 0.0
        // -32767 -> 32767 / 32768, 接近但小于 1.0
        out.push_back(static_cast<float>(value) / 32768.0F);
    }
    return out;
}

}
