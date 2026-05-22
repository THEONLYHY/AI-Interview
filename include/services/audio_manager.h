#ifndef INCLUDE_SERVICES_AUDIO_MANAGER_H_
#define INCLUDE_SERVICES_AUDIO_MANAGER_H_

#include <cstdint>
#include <memory>
#include <vector>

namespace interview::services {

struct Pcm16Level {
    int peak = 0;
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
    AudioManager();
    ~AudioManager();

    AudioManager(const AudioManager&) = delete;
    AudioManager& operator=(const AudioManager&) = delete;

    AudioManager(AudioManager&&) = delete;
    AudioManager& operator=(AudioManager&&) = delete;

    void OpenInputStream();
    std::vector<int16_t> ReadAudio();
    void StopInput();

    void OpenOutputStream();
    void WriteAudio(const std::vector<float>& samples);
    void StopOutput();

    void Cleanup();

    static std::vector<float> Float32PcmLeToSamples(
        const std::vector<uint8_t>& pcm);
    static Pcm16Level Pcm16LeLevel(const std::vector<uint8_t>& pcm);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace interview::services

#endif // INCLUDE_SERVICES_AUDIO_MANAGER_H_
