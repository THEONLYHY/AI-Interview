#include "services/audio_manager.h"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

// PortAudio 的 C API 头文件。
// 这里放在 .cc 实现文件中，而不是放到 audio_manager.h 里，
// 是为了避免项目其他模块直接依赖 PortAudio。
#include <portaudio.h>

#include "common/config.h"
#include "common/logger.h"

namespace interview::services {
namespace {

// Stage 8 的实时对话音频协议是固定的。
//
// 虽然代码仍然会从配置中读取 chunk 大小、采样率等参数，
// 但是真正传给服务端、以及从服务端返回后播放的音频格式，
// 都被固定为 mono int16 PCM。
//
// 这样可以保证：
// 1. 客户端录音发送给服务端的格式稳定；
// 2. 服务端返回的 TTS 音频格式与本地播放逻辑一致；
// 3. 即使本地配置被误改，也不会破坏 Stage 8 的通信协议。
constexpr int kMonoChannels = 1;          // 单声道。
constexpr int kPcmBitSize = 16;           // 每个采样点 16 bit，也就是 int16。
constexpr const char* kPcmFormat = "pcm"; // 原始 PCM 音频格式。

// 判断音频配置是否满足 Stage 8 固定要求：
// - 单声道；
// - 16-bit；
// - pcm 格式。
//
// 参数：
// - config：从配置系统中读取出来的音频配置。
//
// 返回值：
// - true：配置已经是 mono 16-bit PCM；
// - false：配置不符合要求，后续需要强制修正。
bool IsPcm16Mono(const interview::common::AudioConfig& config) {
    return config.channels == kMonoChannels &&
           config.bit_size == kPcmBitSize &&
           config.format == kPcmFormat;
}

// 如果 value 是正数，就返回 value；否则返回 fallback。
//
// 这个函数用于保护配置项。
// 例如配置文件里 sample_rate 或 chunk 被写成 0、负数时，
// 这里会自动回退到合理默认值。
int PositiveOrDefault(int value, int fallback) {
    return value > 0 ? value : fallback;
}

// 把 PortAudio 的错误码转换成人类可读的错误字符串。
//
// PaError 是 PortAudio 的错误码类型。
// Pa_GetErrorText() 是 PortAudio 提供的错误解释函数。
std::string PaErrorText(PaError error) {
    return Pa_GetErrorText(error);
}

}  // namespace

// AudioManager 的真正实现类。
//
// AudioManager 使用 Pimpl 模式：
// - audio_manager.h 中只暴露 AudioManager 和 Impl 的前向声明；
// - 具体实现细节都放在 AudioManager::Impl 中；
// - PortAudio 的 PaStream、PaStreamParameters 等类型不会出现在公共头文件中。
//
// 这样做的好处：
// 1. 减少编译依赖；
// 2. 隐藏第三方库细节；
// 3. 后续替换音频后端或接入 Qt 时更容易维护。
class AudioManager::Impl {
public:
    // 构造函数。
    //
    // 主要流程：
    // 1. 从全局配置中读取输入、输出音频配置；
    // 2. 检查配置是否符合 Stage 8 固定音频格式；
    // 3. 如果配置不符合，就强制改回 mono 16-bit PCM；
    // 4. 为采样率和 chunk 设置默认值；
    // 5. 初始化 PortAudio。
    Impl() {
        // 获取配置系统当前状态的一份快照。
        //
        // 使用 snapshot 的好处是：
        // Impl 后续使用的是这份固定配置副本，
        // 不会因为全局配置对象在别处发生变化而受到影响。
        const auto snapshot = interview::common::Config::Instance().Snapshot();

        // 输入配置通常用于麦克风采集。
        input_config_ = snapshot.audio_input;

        // 输出配置通常用于扬声器 / 耳机播放。
        output_config_ = snapshot.audio_output;

        // 在 PortAudio 边界处统一音频格式。
        //
        // 如果本地配置意外修改了 channels、bit_size 或 format，
        // 这里不会完全相信配置，而是会：
        // 1. 打印 warning 日志，提示配置不符合 Stage 8 协议；
        // 2. 强制改回固定格式。
        //
        // 这样可以避免因为配置错误导致服务端协议、录音路径、播放路径不一致。
        if (!IsPcm16Mono(input_config_)) {
            LOG_WARN("audio_input must be mono 16-bit pcm; forcing fixed Stage 8 format");

            // 输入音频强制使用单声道。
            input_config_.channels = kMonoChannels;

            // 输入音频强制使用 16-bit PCM。
            input_config_.bit_size = kPcmBitSize;

            // 输入音频强制标记为 pcm 格式。
            input_config_.format = kPcmFormat;
        }

        // 输出音频同样必须符合 mono 16-bit PCM。
        //
        // 服务端返回的 TTS 音频会按这个格式解释，
        // 然后再转换成 float32 写入 PortAudio 输出流。
        if (!IsPcm16Mono(output_config_)) {
            LOG_WARN("audio_output must be mono 16-bit pcm; forcing fixed Stage 8 format");

            // 输出音频强制使用单声道。
            output_config_.channels = kMonoChannels;

            // 输出音频强制使用 16-bit PCM。
            output_config_.bit_size = kPcmBitSize;

            // 输出音频强制标记为 pcm 格式。
            output_config_.format = kPcmFormat;
        }

        // 设置输入采样率。
        //
        // 如果配置中的 sample_rate 是正数，就使用配置值；
        // 如果配置非法，例如 0 或负数，就回退到 16000 Hz。
        //
        // 16 kHz 是语音识别 / 实时语音输入中常见的采样率。
        input_config_.sample_rate =
            PositiveOrDefault(input_config_.sample_rate, 16000);

        // 设置输入 chunk 大小。
        //
        // chunk 表示每次 Pa_ReadStream() 读取多少帧。
        // 默认值 160 在 16 kHz 下约等于 10 ms 音频。
        input_config_.chunk = PositiveOrDefault(input_config_.chunk, 160);

        // 设置输出采样率。
        //
        // 如果配置非法，就回退到 24000 Hz。
        // 这里对应服务端 TTS 返回音频的播放采样率。
        output_config_.sample_rate =
            PositiveOrDefault(output_config_.sample_rate, 24000);

        // 设置输出 chunk 大小。
        //
        // 默认值 240 在 24 kHz 下约等于 10 ms 音频。
        output_config_.chunk = PositiveOrDefault(output_config_.chunk, 240);

        // 初始化 PortAudio。
        //
        // 使用 PortAudio 打开任何输入 / 输出流之前，
        // 必须先调用 Pa_Initialize()。
        const PaError err = Pa_Initialize();

        // 初始化失败时，记录错误日志并标记 initialized_ 为 false。
        //
        // 后续 StartInput() / StartOutput() 会先检查 initialized_，
        // 因此这里失败后不会继续打开音频设备。
        if (err != paNoError) {
            LOG_ERROR("PortAudio initialize failed: {}", PaErrorText(err));
            initialized_ = false;
            return;
        }

        // PortAudio 初始化成功。
        initialized_ = true;
    }

    // 析构函数。
    //
    // 负责释放底层资源，顺序是：
    // 1. 停止输入流；
    // 2. 停止输出流；
    // 3. 终止 PortAudio。
    //
    // 析构函数中只记录日志，不抛异常。
    ~Impl() {
        // 如果输入流还开着，先停止并关闭。
        StopInput();

        // 如果输出流还开着，也停止并关闭。
        StopOutput();

        // 只有初始化成功过，才需要调用 Pa_Terminate()。
        if (initialized_) {
            const PaError err = Pa_Terminate();

            // PortAudio 终止失败时记录 warning。
            // 这里不能很好地恢复，只能提示开发者。
            if (err != paNoError) {
                LOG_WARN("PortAudio terminate returned: {}", PaErrorText(err));
            }
        }
    }

    // 启动音频输入。
    //
    // 这个函数会打开系统默认输入设备，通常是默认麦克风。
    // 成功后，可以调用 ReadInputChunk() 阻塞式读取音频数据。
    //
    // 返回值：
    // - true：输入流已经启动或本来就已经启动；
    // - false：启动失败。
    bool StartInput() {
        // PortAudio 没有初始化成功时，不能打开输入流。
        if (!initialized_) {
            return false;
        }

        // 如果 input_stream_ 已经不是 nullptr，说明输入流已经打开。
        // 这里直接返回 true，避免重复打开。
        if (input_stream_ != nullptr) {
            return true;
        }

        // 输入流参数。
        //
        // PaStreamParameters 用于告诉 PortAudio：
        // - 使用哪个设备；
        // - 通道数是多少；
        // - 采样格式是什么；
        // - 建议延迟是多少；
        // - 是否有平台相关的扩展配置。
        PaStreamParameters input_params{};

        // 获取系统默认输入设备。
        input_params.device = Pa_GetDefaultInputDevice();

        // 如果没有默认输入设备，直接失败。
        // 例如机器没有麦克风、麦克风被系统禁用等。
        if (input_params.device == paNoDevice) {
            LOG_ERROR("no default input audio device");
            return false;
        }

        // 查询默认输入设备的信息。
        //
        // 这里主要使用 device_info->defaultLowInputLatency
        // 作为输入流的建议延迟。
        const PaDeviceInfo* device_info = Pa_GetDeviceInfo(input_params.device);
        if (device_info == nullptr) {
            LOG_ERROR("failed to query default input audio device");
            return false;
        }

        // 设置输入通道数。
        // Stage 8 会强制为 1，也就是单声道。
        input_params.channelCount = input_config_.channels;

        // 设置输入采样格式为 paInt16。
        //
        // 这表示 PortAudio 读出来的样本是 int16_t。
        input_params.sampleFormat = paInt16;

        // 使用设备推荐的低输入延迟。
        input_params.suggestedLatency = device_info->defaultLowInputLatency;

        // 不使用平台相关的额外流信息。
        input_params.hostApiSpecificStreamInfo = nullptr;

        // 打开输入流。
        //
        // 参数说明：
        // - &input_stream_：输出参数，成功后得到 PaStream*；
        // - &input_params：输入设备参数；
        // - nullptr：这里没有输出参数，因为这是纯输入流；
        // - input_config_.sample_rate：输入采样率，例如 16000；
        // - input_config_.chunk：每次读写的帧数；
        // - paNoFlag：不使用特殊标志；
        // - nullptr：不使用回调函数；
        // - nullptr：没有回调用户数据。
        //
        // 因为没有使用回调函数，所以这个流会以阻塞方式读取。
        PaError err = Pa_OpenStream(&input_stream_,
                                    &input_params,
                                    nullptr,
                                    input_config_.sample_rate,
                                    input_config_.chunk,
                                    paNoFlag,
                                    nullptr,
                                    nullptr);
        if (err != paNoError) {
            LOG_ERROR("open input stream failed: {}", PaErrorText(err));
            input_stream_ = nullptr;
            return false;
        }

        // 启动输入流。
        //
        // Pa_OpenStream() 只是打开流，Pa_StartStream() 才真正开始工作。
        err = Pa_StartStream(input_stream_);
        if (err != paNoError) {
            LOG_ERROR("start input stream failed: {}", PaErrorText(err));

            // 如果启动失败，需要关闭刚刚打开的流，避免资源泄漏。
            Pa_CloseStream(input_stream_);
            input_stream_ = nullptr;
            return false;
        }

        // 为输入采样缓存分配空间。
        //
        // 缓存大小 = chunk 帧数 * 通道数。
        // 因为 Stage 8 是单声道，所以一般就是 chunk 个 int16_t。
        input_samples_.assign(
            static_cast<std::size_t>(input_config_.chunk * input_config_.channels),
            0);

        // 记录输入流启动信息。
        LOG_INFO("audio input started: {} Hz, chunk={} frames",
                 input_config_.sample_rate, input_config_.chunk);
        return true;
    }

    // 读取一个输入音频块。
    //
    // 参数：
    // - pcm：输出参数，用来接收小端序 int16 PCM 字节流。
    //
    // 返回值：
    // - true：读取成功，pcm 中有本次读取到的数据；
    // - false：读取失败或输入流尚未启动。
    bool ReadInputChunk(std::vector<uint8_t>& pcm) {
        // 输入流没有启动，无法读取。
        if (input_stream_ == nullptr) {
            return false;
        }

        // 从 PortAudio 输入流阻塞读取一个 chunk。
        //
        // input_samples_.data() 是 int16_t 缓冲区。
        // input_config_.chunk 表示读取多少帧。
        const PaError err =
            Pa_ReadStream(input_stream_, input_samples_.data(),
                          input_config_.chunk);

        // 如果发生普通读取错误，返回 false。
        //
        // paInputOverflowed 是特殊情况：
        // 表示输入设备的数据来得太快，中间可能有数据溢出。
        // 但当前缓冲区里仍然可能有可用的最新音频，
        // 所以这里不把它当作致命失败。
        if (err != paNoError && err != paInputOverflowed) {
            LOG_WARN("read input stream failed: {}", PaErrorText(err));
            return false;
        }

        // 输入溢出时记录 debug 日志。
        //
        // 这里仍然继续发送最新 chunk，保证实时对话不中断。
        if (err == paInputOverflowed) {
            LOG_DEBUG("input stream overflowed; sending latest available chunk");
        }

        // 清空输出 PCM 字节数组。
        pcm.clear();

        // 预留足够空间。
        // 每个 int16_t 采样点占 2 个字节。
        pcm.reserve(input_samples_.size() * sizeof(int16_t));

        // 将 PortAudio 返回的 int16_t 样本序列化成小端序字节。
        for (const int16_t sample : input_samples_) {
            // PortAudio 返回的是主机字节序的 int16。
            //
            // 但是实时协议要求 little-endian 小端序字节，
            // 因此这里不能直接把内存 reinterpret_cast 成 uint8_t 数组发送。
            //
            // 显式拆成低字节、高字节，可以保证在大端 / 小端机器上都得到一致结果。
            const auto u = static_cast<uint16_t>(sample);

            // 低 8 位先写入。
            pcm.push_back(static_cast<uint8_t>(u & 0xFF));

            // 高 8 位后写入。
            pcm.push_back(static_cast<uint8_t>((u >> 8) & 0xFF));
        }
        return true;
    }

    // 停止音频输入。
    //
    // 如果输入流已经打开，则停止并关闭它。
    // 如果输入流没有打开，则直接返回。
    void StopInput() {
        // 输入流为空，说明没有可关闭的输入资源。
        if (input_stream_ == nullptr) {
            return;
        }

        // 停止输入流。
        Pa_StopStream(input_stream_);

        // 关闭输入流并释放 PortAudio 流资源。
        Pa_CloseStream(input_stream_);

        // 防止悬空指针。
        input_stream_ = nullptr;

        // 清空输入采样缓存。
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
        // PortAudio 没有初始化成功时，不能打开输出流。
        if (!initialized_) {
            return false;
        }

        // 如果输出流已经打开，直接返回成功。
        if (output_stream_ != nullptr) {
            return true;
        }

        // 输出流参数。
        PaStreamParameters output_params{};

        // 获取系统默认输出设备。
        output_params.device = Pa_GetDefaultOutputDevice();

        // 如果没有默认输出设备，直接失败。
        if (output_params.device == paNoDevice) {
            LOG_ERROR("no default output audio device");
            return false;
        }

        // 查询默认输出设备信息。
        // 这里主要用于获取默认低延迟输出参数。
        const PaDeviceInfo* device_info = Pa_GetDeviceInfo(output_params.device);
        if (device_info == nullptr) {
            LOG_ERROR("failed to query default output audio device");
            return false;
        }

        // 设置输出通道数。
        // Stage 8 会强制为 1，也就是单声道播放。
        output_params.channelCount = output_config_.channels;

        // 输出流使用 float32 格式。
        //
        // 注意：服务端返回的是 int16 PCM，
        // 但播放前会通过 Pcm16LeToFloat32() 转成 float。
        //
        // 这样做的好处：
        // - 归一化后的浮点数据范围明确；
        // - 避免不同平台 int16 输出裁剪行为不一致；
        // - PortAudio 对 float32 输出支持较稳定。
        output_params.sampleFormat = paFloat32;

        // 使用设备推荐的低输出延迟。
        output_params.suggestedLatency = device_info->defaultLowOutputLatency;

        // 不使用平台相关的额外流信息。
        output_params.hostApiSpecificStreamInfo = nullptr;

        // 打开输出流。
        //
        // 参数说明：
        // - &output_stream_：输出参数，成功后得到 PaStream*；
        // - nullptr：这里没有输入参数，因为这是纯输出流；
        // - &output_params：输出设备参数；
        // - output_config_.sample_rate：输出采样率，例如 24000；
        // - output_config_.chunk：每次写入的帧数建议；
        // - paNoFlag：不使用特殊标志；
        // - nullptr：不使用回调函数；
        // - nullptr：没有回调用户数据。
        //
        // 不使用回调函数意味着播放采用阻塞式 Pa_WriteStream()。
        PaError err = Pa_OpenStream(&output_stream_,
                                    nullptr,
                                    &output_params,
                                    output_config_.sample_rate,
                                    output_config_.chunk,
                                    paNoFlag,
                                    nullptr,
                                    nullptr);
        if (err != paNoError) {
            LOG_ERROR("open output stream failed: {}", PaErrorText(err));
            output_stream_ = nullptr;
            return false;
        }

        // 启动输出流。
        err = Pa_StartStream(output_stream_);
        if (err != paNoError) {
            LOG_ERROR("start output stream failed: {}", PaErrorText(err));

            // 如果启动失败，需要关闭已经打开的流，避免资源泄漏。
            Pa_CloseStream(output_stream_);
            output_stream_ = nullptr;
            return false;
        }

        LOG_INFO("audio output started: {} Hz, chunk={} frames",
                 output_config_.sample_rate, output_config_.chunk);
        return true;
    }

    // 播放一段服务端返回的 TTS PCM 音频。
    //
    // 参数：
    // - pcm：服务端返回的 int16 little-endian PCM 字节流。
    //
    // 返回值：
    // - true：写入播放成功，或者数据转换后为空但不需要播放；
    // - false：输出流未启动、输入为空，或者写入 PortAudio 失败。
    bool PlayTtsPcm(const std::vector<uint8_t>& pcm) {
        // 输出流没有启动，或者传入数据为空时，不进行播放。
        if (output_stream_ == nullptr || pcm.empty()) {
            return false;
        }

        // 将 int16 little-endian PCM 转换为 float32。
        //
        // PortAudio 输出流使用 paFloat32，
        // 因此这里必须先做格式转换。
        std::vector<float> samples = AudioManager::Pcm16LeToFloat32(pcm);

        // 如果转换结果为空，说明输入没有完整的 int16 采样点。
        //
        // 例如 pcm 只有 1 个字节，无法组成一个 int16。
        // 这里返回 true，表示没有真正的播放错误，只是没有可播放数据。
        if (samples.empty()) {
            return true;
        }

        // 计算需要写入的帧数。
        //
        // 帧数 = 样本数 / 通道数。
        // Stage 8 是单声道，因此通常 frames == samples.size()。
        const auto frames =
            static_cast<unsigned long>(samples.size() / output_config_.channels);

        // 阻塞式写入 PortAudio 输出流。
        //
        // samples.data() 是 float32 样本数组。
        // frames 是要写入的音频帧数。
        const PaError err =
            Pa_WriteStream(output_stream_, samples.data(), frames);

        // paOutputUnderflowed 表示输出下溢，
        // 也就是播放端没有及时拿到足够数据，可能造成轻微卡顿。
        //
        // 这里不把它当作致命错误，避免播放线程因为一次下溢而停止。
        if (err != paNoError && err != paOutputUnderflowed) {
            LOG_WARN("write output stream failed: {}", PaErrorText(err));
            return false;
        }
        return true;
    }

    // 停止音频输出。
    //
    // 如果输出流已经打开，则停止并关闭它。
    // 如果输出流没有打开，则直接返回。
    void StopOutput() {
        // 输出流为空，说明没有可关闭的输出资源。
        if (output_stream_ == nullptr) {
            return;
        }

        // 停止输出流。
        Pa_StopStream(output_stream_);

        // 关闭输出流并释放 PortAudio 流资源。
        Pa_CloseStream(output_stream_);

        // 防止悬空指针。
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

    // 输出音频配置。
    //
    // 主要包含：
    // - sample_rate：采样率，例如 24000；
    // - chunk：每次写入的帧数建议，例如 240；
    // - channels：通道数，Stage 8 强制为 1；
    // - bit_size：位深，Stage 8 强制为 16；
    // - format：格式，Stage 8 强制为 pcm。
    interview::common::AudioConfig output_config_;

    // PortAudio 输入流指针。
    //
    // nullptr 表示输入流未打开。
    // 非 nullptr 表示输入流已经由 Pa_OpenStream() 创建。
    PaStream* input_stream_ = nullptr;

    // PortAudio 输出流指针。
    //
    // nullptr 表示输出流未打开。
    // 非 nullptr 表示输出流已经由 Pa_OpenStream() 创建。
    PaStream* output_stream_ = nullptr;

    // 输入采样缓存。
    //
    // Pa_ReadStream() 会把 int16 音频样本写到这个 vector 中。
    // 随后 ReadInputChunk() 会把这些 int16 样本显式序列化成小端序字节。
    std::vector<int16_t> input_samples_;

    // PortAudio 是否初始化成功。
    //
    // StartInput() 和 StartOutput() 都会先检查这个标志。
    bool initialized_ = false;
};

// AudioManager 构造函数。
//
// 创建 Impl 对象，真正的 PortAudio 初始化和配置读取都在 Impl 构造函数中完成。
AudioManager::AudioManager() : impl_(std::make_unique<Impl>()) {}

// AudioManager 析构函数。
//
// 使用 default 即可。
// std::unique_ptr<Impl> 会自动销毁 Impl，
// Impl 析构时会关闭输入 / 输出流并终止 PortAudio。
AudioManager::~AudioManager() = default;

// 启动输入。
//
// 这是对 Impl::StartInput() 的简单转发。
bool AudioManager::StartInput() {
    return impl_->StartInput();
}

// 读取一个输入音频块。
//
// 这是对 Impl::ReadInputChunk() 的简单转发。
bool AudioManager::ReadInputChunk(std::vector<uint8_t>& pcm) {
    return impl_->ReadInputChunk(pcm);
}

// 停止输入。
//
// 这是对 Impl::StopInput() 的简单转发。
void AudioManager::StopInput() {
    impl_->StopInput();
}

// 启动输出。
//
// 这是对 Impl::StartOutput() 的简单转发。
bool AudioManager::StartOutput() {
    return impl_->StartOutput();
}

// 播放 TTS PCM 数据。
//
// 这是对 Impl::PlayTtsPcm() 的简单转发。
bool AudioManager::PlayTtsPcm(const std::vector<uint8_t>& pcm) {
    return impl_->PlayTtsPcm(pcm);
}

// 停止输出。
//
// 这是对 Impl::StopOutput() 的简单转发。
void AudioManager::StopOutput() {
    impl_->StopOutput();
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
std::vector<float> AudioManager::Pcm16LeToFloat32(
    const std::vector<uint8_t>& pcm) {
    std::vector<float> out;

    // 每两个字节才能组成一个 int16_t，
    // 所以输出样本数量最多是 pcm.size() / 2。
    out.reserve(pcm.size() / sizeof(int16_t));

    // 每次处理两个字节。
    //
    // 条件 i + 1 < pcm.size() 可以保证存在完整的两个字节。
    // 如果 pcm 最后只剩一个不完整字节，会被自然忽略。
    for (std::size_t i = 0; i + 1 < pcm.size(); i += 2) {
        // little-endian 的低字节。
        const int lo = pcm[i];

        // little-endian 的高字节，左移 8 位。
        const int hi = pcm[i + 1] << 8;

        // 合并成 16-bit 数值。
        //
        // 此时 value 的范围是 0 到 65535，
        // 还没有转换成有符号 int16。
        int value = lo | hi;

        // 如果最高位为 1，说明这是一个负数的二进制补码表示。
        //
        // 例如：
        // - 0x8000 应解释为 -32768；
        // - 0xFFFF 应解释为 -1。
        //
        // 通过减去 0x10000，把无符号范围 32768..65535
        // 映射到有符号范围 -32768..-1。
        if (value >= 0x8000) {
            value -= 0x10000;
        }

        // 将 int16 范围归一化到 float。
        //
        // 除以 32768.0F 后：
        // - -32768 -> -1.0；
        // - 0 -> 0.0；
        // - 32767 -> 32767 / 32768，接近但小于 1.0。
        out.push_back(static_cast<float>(value) / 32768.0F);
    }
    return out;
}

}  // namespace interview::services
