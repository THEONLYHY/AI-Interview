#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

#include "common/config.h"
#include "services/audio_manager.h"

namespace {

std::vector<uint8_t> Float32LeBytes(std::initializer_list<float> samples) {
    std::vector<uint8_t> bytes;
    bytes.reserve(samples.size() * sizeof(float));
    for (float sample : samples) {
        uint32_t raw = 0;
        static_assert(sizeof(raw) == sizeof(sample));
        std::memcpy(&raw, &sample, sizeof(sample));
        bytes.push_back(static_cast<uint8_t>(raw & 0xFF));
        bytes.push_back(static_cast<uint8_t>((raw >> 8) & 0xFF));
        bytes.push_back(static_cast<uint8_t>((raw >> 16) & 0xFF));
        bytes.push_back(static_cast<uint8_t>((raw >> 24) & 0xFF));
    }
    return bytes;
}

void Float32PcmLeToSamplesPreservesRawTtsSamples() {
    std::vector<uint8_t> pcm = Float32LeBytes({0.25F, -0.5F, 0.0F});
    pcm.push_back(0xAA);

    const std::vector<float> out =
        interview::services::AudioManager::Float32PcmLeToSamples(pcm);

    assert(out.size() == 3u);
    assert(std::fabs(out[0] - 0.25F) < 0.00001F);
    assert(std::fabs(out[1] - -0.5F) < 0.00001F);
    assert(out[2] == 0.0F);
}

void Pcm16LeLevelReportsPeakAndRms() {
    const std::vector<uint8_t> pcm = {
        0x00, 0x00,
        0xB8, 0x0B,
        0x60, 0xF0,
        0x00, 0x00,
    };

    const auto level =
        interview::services::AudioManager::Pcm16LeLevel(pcm);

    assert(level.peak == 4000);
    assert(std::fabs(level.rms - 2500.0) < 0.00001);
}

void NestedAudioConfigLoadsArchiveShape() {
    const std::filesystem::path path =
        std::filesystem::temp_directory_path() /
        "ai_interview_audio_config_test.json";
    {
        std::ofstream file(path);
        file << R"json({
            "audio": {
                "input": {
                    "chunk": 3200,
                    "channels": 1,
                    "sample_rate": 16000,
                    "bit_size": 16,
                    "format": "pcm"
                },
                "output": {
                    "chunk": 4800,
                    "channels": 1,
                    "sample_rate": 24000,
                    "bit_size": 16,
                    "format": "pcm"
                }
            }
        })json";
    }

    auto& config = interview::common::Config::Instance();
    assert(config.LoadFromFile(path.string()));
    const interview::common::AppConfig snapshot = config.Snapshot();
    assert(snapshot.audio_input.chunk == 3200);
    assert(snapshot.audio_input.sample_rate == 16000);
    assert(snapshot.audio_output.chunk == 4800);
    assert(snapshot.audio_output.sample_rate == 24000);

    std::filesystem::remove(path);
}

void StartSessionPayloadUsesRealtimeNestedAudioConfigShape() {
    const nlohmann::json payload =
        interview::common::Config::Instance().BuildStartSessionPayload();

    assert(payload["tts"].contains("speaker"));
    assert(payload["tts"].contains("audio_config"));
    assert(payload["tts"]["audio_config"]["format"] == "pcm");
    assert(payload["tts"]["audio_config"]["sample_rate"] == 24000);
    assert(!payload["tts"].contains("format"));
    assert(!payload["tts"].contains("sample_rate"));

    assert(payload["asr"].contains("extra"));
    assert(payload["asr"]["extra"].contains("vad_silence_duration"));
    assert(payload["dialog"].contains("location"));
    assert(payload["dialog"].contains("extra"));
    assert(payload["dialog"]["extra"].contains("input_mod"));

    const std::string system_role =
        payload["dialog"]["system_role"].get<std::string>();
    assert(system_role.find("Do not answer technical questions") !=
           std::string::npos);
    assert(system_role.find("read the exact text") != std::string::npos);
}

}  // namespace

int main() {
    Float32PcmLeToSamplesPreservesRawTtsSamples();
    Pcm16LeLevelReportsPeakAndRms();
    NestedAudioConfigLoadsArchiveShape();
    StartSessionPayloadUsesRealtimeNestedAudioConfigShape();
    return 0;
}
