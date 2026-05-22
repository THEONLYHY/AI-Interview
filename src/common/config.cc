#include "common/config.h"

#include <fstream>
#include <stdexcept>
#include <utility> 


#include <nlohmann/json.hpp>
#include "common/logger.h"



namespace interview::common {
namespace {

constexpr const char* kRealtimeReadAloudGuard =
    "\n\nRealtime voice policy:\n"
    "- Do not answer technical questions or explain concepts.\n"
    "- When the client sends a text query, read the exact text only.\n"
    "- Do not add explanations, examples, prefixes, suffixes, or summaries.\n"
    "- If candidate speech is recognized, give at most a short acknowledgement "
    "and wait for the client to send the next prompt.";

std::string WithRealtimeReadAloudGuard(const std::string& system_role) {
    constexpr const char* kMarker = "Realtime voice policy:";
    if (system_role.find(kMarker) != std::string::npos) {
        return system_role;
    }
    return system_role + kRealtimeReadAloudGuard;
}

void LoadWsConfig(const nlohmann::json& root, AppConfig& config) {
    // 如果配置文件中没有 "ws" 字段，说明用户没有配置WebSocket
    // 这里直接返回，保留AppConfig 中已有的默认值
    if (!root.contains("ws")) {
        return;
    }

    // 取出"ws" 子对象
    const nlohmann::json& ws = root["ws"];

    // 读取base_url
    config.ws.base_url = ws.value("base_url", config.ws.base_url);

    if(!ws.contains("headers")) {
        return;
    }

    // 取出 headers 子对象
    const nlohmann::json& headers = ws["headers"];

    // 下面依次读取各个 WebSocket 请求头字段。
    // 如果 JSON 中没有对应字段，就保留默认值。
    config.ws.headers.api_app_id =
        headers.value("X-Api-App-ID", config.ws.headers.api_app_id);

    config.ws.headers.api_access_key =
        headers.value("X-Api-Access-Key", config.ws.headers.api_access_key);

    config.ws.headers.api_resource_id =
        headers.value("X-Api-Resource-Id", config.ws.headers.api_resource_id);

    config.ws.headers.api_app_key =
        headers.value("X-Api-App-Key", config.ws.headers.api_app_key);

    config.ws.headers.api_connect_id =
        headers.value("X-Api-Connect-Id", config.ws.headers.api_connect_id);
}

/**
 * 加载 LLM，大模型相关配置。
 *
 * JSON 结构大概类似：
 *
 * {
 *   "llm": {
 *     "api_url": "https://api.xxx.com/chat",
 *     "api_key": "sk-xxx",
 *     "model": "xxx-model",
 *     "temperature": 0.7,
 *     "max_tokens": 1024,
 *     "timeout_seconds": 30
 *   }
 * }
 */
void LoadLlmConfig(const nlohmann::json& root, AppConfig& config) {
    // 如果没有 llm 字段，直接使用默认 LLM 配置。
    if (!root.contains("llm")) {
        return;
    }

    const nlohmann::json& llm = root["llm"];

    // 依次读取大模型 API 地址、密钥、模型名、温度、最大 token 数和超时时间。
    // 每一项都使用 value(key, default_value)，这样可以支持部分配置。
    config.llm.api_url = llm.value("api_url", config.llm.api_url);
    config.llm.api_key = llm.value("api_key", config.llm.api_key);
    config.llm.model = llm.value("model", config.llm.model);
    config.llm.temperature = llm.value("temperature", config.llm.temperature);
    config.llm.max_tokens = llm.value("max_tokens", config.llm.max_tokens);
    config.llm.timeout_seconds =
        llm.value("timeout_seconds", config.llm.timeout_seconds);
}

/**
 * 加载对话相关配置。
 *
 * JSON 结构大概类似：
 *
 * {
 *   "dialog": {
 *     "bot_name": "面试官",
 *     "system_role": "你是一名 C++ 面试官",
 *     "speaking_style": "专业、简洁",
 *     "city": "Singapore",
 *     "strict_audio": true,
 *     "audit_response": false,
 *     "recv_timeout": 30,
 *     "input_mod": "audio"
 *   }
 * }
 */

void LoadDialogConfig(const nlohmann::json& root, AppConfig& config) {
    // 如果没有 dialog 字段，直接使用默认对话配置。
    if (!root.contains("dialog")) {
        return;
    }

    const nlohmann::json& dialog = root["dialog"];

    // bot_name：机器人名字，例如“AI 面试官”
    config.dialog.bot_name = dialog.value("bot_name", config.dialog.bot_name);

    // system_role：系统角色提示词，通常用于控制大模型的整体行为。
    config.dialog.system_role =
        dialog.value("system_role", config.dialog.system_role);

    // speaking_style：说话风格，例如“严肃”“鼓励式”“简洁”等。
    config.dialog.speaking_style =
        dialog.value("speaking_style", config.dialog.speaking_style);

    // city：城市信息，可能用于本地化表达或者业务上下文。
    config.dialog.city = dialog.value("city", config.dialog.city);

    // strict_audio：是否严格要求音频输入。
    config.dialog.strict_audio =
        dialog.value("strict_audio", config.dialog.strict_audio);

    // audit_response：是否对模型回复进行审核。
    config.dialog.audit_response =
        dialog.value("audit_response", config.dialog.audit_response);

    // recv_timeout：接收消息超时时间。
    config.dialog.recv_timeout =
        dialog.value("recv_timeout", config.dialog.recv_timeout);

    // input_mod：输入模式，例如 audio/text。
    config.dialog.input_mod = dialog.value("input_mod", config.dialog.input_mod);
}

/**
 * 加载 ASR，也就是语音识别相关配置。
 *
 * ASR = Automatic Speech Recognition
 *
 * JSON 结构大概类似：
 *
 * {
 *   "asr": {
 *     "end_smooth_window_ms": 800,
 *     "vad_silence_duration": 1000,
 *     "vad_speech_trigger_duration": 200
 *   }
 * }
 */
void LoadAsrConfig(const nlohmann::json& root, AppConfig& config) {
    // 如果没有 asr 字段，直接使用默认语音识别配置。
    if (!root.contains("asr")) {
        return;
    }

    const nlohmann::json& asr = root["asr"];

    // end_smooth_window_ms：
    // 语音结束平滑窗口，单位通常是毫秒。
    // 用来避免一句话中短暂停顿被误判为“说话结束”。
    config.asr.end_smooth_window_ms =
        asr.value("end_smooth_window_ms", config.asr.end_smooth_window_ms);

    // vad_silence_duration：
    // VAD 判断静音持续多长时间后，认为用户停止说话。
    //
    // VAD = Voice Activity Detection，语音活动检测。
    config.asr.vad_silence_duration =
        asr.value("vad_silence_duration", config.asr.vad_silence_duration);

    // vad_speech_trigger_duration：
    // 检测到语音持续多长时间后，认为用户开始说话。
    config.asr.vad_speech_trigger_duration =
        asr.value("vad_speech_trigger_duration",
                  config.asr.vad_speech_trigger_duration);
}

/**
 * 加载 TTS，也就是语音合成相关配置。
 *
 * TTS = Text To Speech
 *
 * JSON 结构大概类似：
 *
 * {
 *   "tts": {
 *     "speaker": "xxx",
 *     "channel": 1,
 *     "format": "pcm",
 *     "sample_rate": 16000
 *   }
 * }
 */
void LoadTtsConfig(const nlohmann::json& root, AppConfig& config) {
    // 如果没有 tts 字段，直接使用默认语音合成配置。
    if (!root.contains("tts")) {
        return;
    }

    const nlohmann::json& tts = root["tts"];

    // speaker：发音人，例如某个音色 ID。
    config.tts.speaker = tts.value("speaker", config.tts.speaker);

    // channel：声道数，例如 1 表示单声道，2 表示双声道。
    config.tts.channel = tts.value("channel", config.tts.channel);

    // format：音频格式，例如 pcm、wav、mp3 等。
    config.tts.format = tts.value("format", config.tts.format);

    // sample_rate：采样率，例如 16000、24000、44100。
    config.tts.sample_rate = tts.value("sample_rate", config.tts.sample_rate);
}

/**
 * 加载音频输入和音频输出配置。
 *
 * JSON 结构大概类似：
 *
 * {
 *   "audio_input": {
 *     "device": "default",
 *     "channel": 1,
 *     "sample_rate": 16000,
 *     "frames_per_buffer": 512
 *   },
 *   "audio_output": {
 *     "device": "default",
 *     "channel": 1,
 *     "sample_rate": 16000,
 *     "frames_per_buffer": 512
 *   }
 * }
 */
void LoadAudioSection(const nlohmann::json& audio, AudioConfig& config) {
    config.chunk = audio.value("chunk",
                               audio.value("frames_per_buffer", config.chunk));
    config.channels = audio.value("channels",
                                  audio.value("channel", config.channels));
    config.sample_rate = audio.value("sample_rate", config.sample_rate);
    config.bit_size = audio.value("bit_size", config.bit_size);
    config.format = audio.value("format", config.format);
}

void LoadAudioConfig(const nlohmann::json& root, AppConfig& config) {
    if (root.contains("audio") && root["audio"].is_object()) {
        const nlohmann::json& audio = root["audio"];
        if (audio.contains("input")) {
            LoadAudioSection(audio["input"], config.audio_input);
        }
        if (audio.contains("output")) {
            LoadAudioSection(audio["output"], config.audio_output);
        }
    }

    if (root.contains("audio_input")) {
        LoadAudioSection(root["audio_input"], config.audio_input);
    }

    if (root.contains("audio_output")) {
        LoadAudioSection(root["audio_output"], config.audio_output);
    }
}

/**
 * 解析整个 JSON 配置对象，生成 AppConfig。
 *
 * 这里的设计思想是：
 *   1. 先创建一个带默认值的 AppConfig；
 *   2. 再用 JSON 文件中的字段覆盖默认值；
 *   3. 没有出现在 JSON 文件中的字段继续保留默认值。
 */

AppConfig ParseConfig(const nlohmann::json& root) {
    // 创建默认配置。
    // AppConfig 的默认值应该在结构体成员默认值或者构造函数里定义。
    AppConfig config;

    // 分模块加载配置。
    LoadWsConfig(root, config);
    LoadLlmConfig(root, config);
    LoadDialogConfig(root, config);
    LoadAsrConfig(root, config);
    LoadTtsConfig(root, config);
    LoadAudioConfig(root, config);

    // 返回最终配置。
    return config;
}

} // namespace

/**
 * 获取 Config 单例对象。
 *
 * static 局部变量在 C++11 之后是线程安全初始化的。
 * 也就是说，多线程同时第一次调用 Instance() 时，
 * C++ 标准保证 instance 只会被初始化一次。
 */
Config& Config::Instance() {
    static Config instance;
    return instance;
}

/**
 * 从指定文件路径加载配置。
 *
 * 返回值：
 *   true  表示加载成功；
 *   false 表示加载失败。
 *
 * 加载失败的情况包括：
 *   1. 文件打不开；
 *   2. JSON 格式错误；
 *   3. 字段类型不匹配；
 *   4. 解析过程中抛出其他异常。
 */

bool Config::LoadFromFile(const std::string& file_path) {
    try {
        //
        std::ifstream ifs(file_path);

        if (!ifs.is_open()) {
            return false;
        }

        nlohmann::json root;
        ifs >> root;
        AppConfig parsed = ParseConfig(root);

        std::lock_guard<std::mutex> lock(mutex_);
        config_ = std::move(parsed);
        return true;        
    } catch (const std::exception& e) {
        LOG_ERROR("failed to load config file {} : {}", file_path, e.what());
        return false;
    }
}

/**
 * 构造启动会话需要发送的 JSON payload。
 *
 * 这个函数通常用于 WebSocket 或 HTTP 请求：
 *   当客户端启动一次语音/对话会话时，
 *   需要把 dialog、asr、tts 相关配置发送给服务端。
 */
nlohmann::json Config::BuildStartSessionPayload() const {
    std::lock_guard<std::mutex> lock(mutex_);

    return {
        {"dialog",
         {
             {"bot_name", config_.dialog.bot_name},
             {"system_role",
              WithRealtimeReadAloudGuard(config_.dialog.system_role)},
             {"speaking_style", config_.dialog.speaking_style},
             {"location", {{"city", config_.dialog.city}}},
             {"extra",
              {
                  {"strict_audit", config_.dialog.strict_audio},
                  {"audit_response", config_.dialog.audit_response},
                  {"recv_timeout", config_.dialog.recv_timeout},
                  {"input_mod", config_.dialog.input_mod},
              }},
         }},
        {"asr",
         {
             {"extra",
              {
                  {"end_smooth_window_ms", config_.asr.end_smooth_window_ms},
                  {"vad_silence_duration", config_.asr.vad_silence_duration},
                  {"vad_speech_trigger_duration",
                   config_.asr.vad_speech_trigger_duration},
              }},
         }},
        {"tts",
         {
             {"speaker", config_.tts.speaker},
             {"audio_config",
              {
                  {"channel", config_.tts.channel},
                  {"format", config_.tts.format},
                  {"sample_rate", config_.tts.sample_rate},
              }},
         }},
    };
}

/**
 * 获取当前配置快照。
 *
 * 为什么叫 Snapshot？
 *
 * 因为它返回的是 config_ 的一份拷贝，
 * 调用者拿到的是某一时刻的配置副本。
 * 后续 Config 内部配置如果被重新加载，
 * 不会影响已经返回出去的 AppConfig 副本。
 */
AppConfig Config::Snapshot() const {
    // 加锁读取config
    std::lock_guard<std::mutex> lock(mutex_);

    return config_;
}

/**
 * 便捷函数：加载配置文件并返回 AppConfig。
 *
 * 这个函数封装了：
 *   1. 获取 Config 单例；
 *   2. 从文件加载配置；
 *   3. 加载失败则抛异常；
 *   4. 加载成功则返回配置快照。
 */
AppConfig LoadConfig(const std::string& file_path) {
    Config& config = Config::Instance();

    if (!config.LoadFromFile(file_path)) {
        throw std::runtime_error("failed to load config file: " + file_path);
    }
    return config.Snapshot();
}

}  // namespace interview::common
