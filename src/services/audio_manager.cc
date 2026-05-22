#include "services/audio_manager.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include <portaudio.h>

#include "common/config.h"
#include "common/logger.h"

namespace interview::services {
namespace {

constexpr int kMonoChannels = 1;
constexpr int kPcmBitSize = 16;
constexpr const char* kPcmFormat = "pcm";
constexpr int kDefaultInputSampleRate = 16000;
constexpr int kDefaultInputChunk = 3200;
constexpr int kDefaultOutputSampleRate = 24000;
constexpr int kDefaultOutputChunk = 4800;

int PositiveOrDefault(int value, int fallback) {
    return value > 0 ? value : fallback;
}

std::string PaErrorText(PaError error) {
    return Pa_GetErrorText(error);
}

void NormalizeInputConfig(interview::common::AudioConfig& config) {
    if (config.channels != kMonoChannels || config.bit_size != kPcmBitSize ||
        config.format != kPcmFormat) {
        LOG_WARN("audio_input must be mono 16-bit pcm; forcing fixed format");
    }
    config.channels = kMonoChannels;
    config.bit_size = kPcmBitSize;
    config.format = kPcmFormat;
    config.sample_rate =
        PositiveOrDefault(config.sample_rate, kDefaultInputSampleRate);
    config.chunk = PositiveOrDefault(config.chunk, kDefaultInputChunk);
}

void NormalizeOutputConfig(interview::common::AudioConfig& config) {
    if (config.channels != kMonoChannels || config.format != kPcmFormat) {
        LOG_WARN("audio_output must be mono pcm; forcing fixed format");
    }
    config.channels = kMonoChannels;
    config.bit_size = kPcmBitSize;
    config.format = kPcmFormat;
    config.sample_rate =
        PositiveOrDefault(config.sample_rate, kDefaultOutputSampleRate);
    config.chunk = PositiveOrDefault(config.chunk, kDefaultOutputChunk);
    if (config.chunk < kDefaultOutputChunk) {
        LOG_INFO("audio_output.chunk={} is too small for stable blocking "
                 "TTS playback; using {} frames",
                 config.chunk, kDefaultOutputChunk);
        config.chunk = kDefaultOutputChunk;
    }
}

void ThrowIfPaError(PaError err, const std::string& operation) {
    if (err != paNoError) {
        throw std::runtime_error(operation + ": " + PaErrorText(err));
    }
}

}  // namespace

class AudioManager::Impl {
public:
    Impl() {
        const auto snapshot = interview::common::Config::Instance().Snapshot();
        input_config_ = snapshot.audio_input;
        output_config_ = snapshot.audio_output;
        NormalizeInputConfig(input_config_);
        NormalizeOutputConfig(output_config_);

        const PaError err = Pa_Initialize();
        if (err != paNoError) {
            LOG_ERROR("PortAudio initialize failed: {}", PaErrorText(err));
            initialized_ = false;
            return;
        }
        initialized_ = true;
        LogPortAudioDevices();
    }

    ~Impl() {
        Cleanup();
    }

    void OpenInputStream() {
        if (!initialized_) {
            throw std::runtime_error("PortAudio is not initialized");
        }
        if (input_stream_ != nullptr) {
            return;
        }

        PaStreamParameters params{};
        params.device = Pa_GetDefaultInputDevice();
        if (params.device == paNoDevice) {
            throw std::runtime_error("no default input audio device");
        }

        const PaDeviceInfo* device_info = Pa_GetDeviceInfo(params.device);
        if (device_info == nullptr) {
            throw std::runtime_error("failed to query default input audio device");
        }

        params.channelCount = input_config_.channels;
        params.sampleFormat = paInt16;
        params.suggestedLatency = device_info->defaultLowInputLatency;
        params.hostApiSpecificStreamInfo = nullptr;

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
            err = Pa_StartStream(input_stream_);
            ThrowIfPaError(err, "start input stream failed");
        } catch (...) {
            Pa_CloseStream(input_stream_);
            input_stream_ = nullptr;
            throw;
        }

        input_samples_.assign(
            static_cast<std::size_t>(input_config_.chunk * input_config_.channels),
            0);
        LOG_INFO("audio input started: {} Hz, chunk={} frames",
                 input_config_.sample_rate, input_config_.chunk);
    }

    std::vector<int16_t> ReadAudio() {
        if (input_stream_ == nullptr) {
            return {};
        }

        const PaError err =
            Pa_ReadStream(input_stream_, input_samples_.data(),
                          input_config_.chunk);
        if (err == paInputOverflowed) {
            LOG_WARN("input stream overflowed; sending latest available chunk");
        } else if (err != paNoError) {
            LOG_WARN("read input stream failed: {}", PaErrorText(err));
            return {};
        }
        return input_samples_;
    }

    void StopInput() {
        if (input_stream_ == nullptr) {
            return;
        }

        PaError err = Pa_StopStream(input_stream_);
        if (err != paNoError && err != paStreamIsStopped) {
            LOG_WARN("failed to stop input stream: {}", PaErrorText(err));
        }

        err = Pa_CloseStream(input_stream_);
        if (err != paNoError) {
            LOG_WARN("failed to close input stream: {}", PaErrorText(err));
        }

        input_stream_ = nullptr;
        input_samples_.clear();
        LOG_INFO("audio input stopped");
    }

    void OpenOutputStream() {
        if (!initialized_) {
            throw std::runtime_error("PortAudio is not initialized");
        }
        if (output_stream_ != nullptr) {
            return;
        }

        PaStreamParameters params{};
        params.device = Pa_GetDefaultOutputDevice();
        if (params.device == paNoDevice) {
            throw std::runtime_error("no default output audio device");
        }

        const PaDeviceInfo* device_info = Pa_GetDeviceInfo(params.device);
        if (device_info == nullptr) {
            throw std::runtime_error("failed to query default output audio device");
        }

        params.channelCount = output_config_.channels;
        params.sampleFormat = paFloat32;
        params.suggestedLatency = device_info->defaultHighOutputLatency;
        params.hostApiSpecificStreamInfo = nullptr;

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
            err = Pa_StartStream(output_stream_);
            ThrowIfPaError(err, "start output stream failed");
        } catch (...) {
            Pa_CloseStream(output_stream_);
            output_stream_ = nullptr;
            throw;
        }

        LOG_INFO("audio output started: {} Hz, chunk={} frames",
                 output_config_.sample_rate, output_config_.chunk);
    }

    void WriteAudio(const std::vector<float>& samples) {
        if (output_stream_ == nullptr) {
            throw std::runtime_error("output audio stream is not open");
        }
        if (samples.empty()) {
            return;
        }

        const auto frames =
            static_cast<unsigned long>(samples.size() / output_config_.channels);
        if (frames == 0) {
            return;
        }

        const PaError err =
            Pa_WriteStream(output_stream_, samples.data(), frames);
        if (err == paOutputUnderflowed) {
            LOG_WARN("output stream underflowed");
        } else if (err != paNoError) {
            throw std::runtime_error("write output stream failed: " +
                                     PaErrorText(err));
        }
    }

    void StopOutput() {
        if (output_stream_ == nullptr) {
            return;
        }

        PaError err = Pa_StopStream(output_stream_);
        if (err != paNoError && err != paStreamIsStopped) {
            LOG_WARN("failed to stop output stream: {}", PaErrorText(err));
        }

        err = Pa_CloseStream(output_stream_);
        if (err != paNoError) {
            LOG_WARN("failed to close output stream: {}", PaErrorText(err));
        }

        output_stream_ = nullptr;
        LOG_INFO("audio output stopped");
    }

    void Cleanup() {
        StopInput();
        StopOutput();
        if (!initialized_) {
            return;
        }

        const PaError err = Pa_Terminate();
        if (err != paNoError) {
            LOG_WARN("PortAudio terminate returned: {}", PaErrorText(err));
        }
        initialized_ = false;
    }

private:
    void LogPortAudioDevices() const {
        const int host_api_count = Pa_GetHostApiCount();
        const int device_count = Pa_GetDeviceCount();
        const PaDeviceIndex default_in = Pa_GetDefaultInputDevice();
        const PaDeviceIndex default_out = Pa_GetDefaultOutputDevice();
        LOG_INFO("PortAudio diag: host_api_count={}, device_count={}, "
                 "default_input={}, default_output={}",
                 host_api_count, device_count, static_cast<int>(default_in),
                 static_cast<int>(default_out));

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

    interview::common::AudioConfig input_config_;
    interview::common::AudioConfig output_config_;
    PaStream* input_stream_ = nullptr;
    PaStream* output_stream_ = nullptr;
    std::vector<int16_t> input_samples_;
    bool initialized_ = false;
};

AudioManager::AudioManager() : impl_(std::make_unique<Impl>()) {}

AudioManager::~AudioManager() = default;

void AudioManager::OpenInputStream() {
    impl_->OpenInputStream();
}

std::vector<int16_t> AudioManager::ReadAudio() {
    return impl_->ReadAudio();
}

void AudioManager::StopInput() {
    impl_->StopInput();
}

void AudioManager::OpenOutputStream() {
    impl_->OpenOutputStream();
}

void AudioManager::WriteAudio(const std::vector<float>& samples) {
    impl_->WriteAudio(samples);
}

void AudioManager::StopOutput() {
    impl_->StopOutput();
}

void AudioManager::Cleanup() {
    impl_->Cleanup();
}

std::vector<float> AudioManager::Float32PcmLeToSamples(
    const std::vector<uint8_t>& pcm) {
    std::vector<float> out;
    out.reserve(pcm.size() / sizeof(float));

    for (std::size_t i = 0; i + sizeof(float) <= pcm.size(); i += sizeof(float)) {
        uint32_t raw = static_cast<uint32_t>(pcm[i]) |
                       (static_cast<uint32_t>(pcm[i + 1]) << 8) |
                       (static_cast<uint32_t>(pcm[i + 2]) << 16) |
                       (static_cast<uint32_t>(pcm[i + 3]) << 24);
        float sample = 0.0F;
        static_assert(sizeof(sample) == sizeof(raw));
        std::memcpy(&sample, &raw, sizeof(sample));
        out.push_back(sample);
    }
    return out;
}

Pcm16Level AudioManager::Pcm16LeLevel(const std::vector<uint8_t>& pcm) {
    Pcm16Level level;
    double sum_squares = 0.0;
    std::size_t count = 0;

    for (std::size_t i = 0; i + 1 < pcm.size(); i += 2) {
        int value = static_cast<int>(pcm[i]) |
                    (static_cast<int>(pcm[i + 1]) << 8);
        if (value >= 0x8000) {
            value -= 0x10000;
        }

        const int abs_value = value == -32768 ? 32768 : std::abs(value);
        level.peak = std::max(level.peak, abs_value);
        sum_squares += static_cast<double>(value) * value;
        ++count;
    }

    if (count > 0) {
        level.rms = std::sqrt(sum_squares / static_cast<double>(count));
    }
    return level;
}

}  // namespace interview::services
