#ifndef INCLUDE_CONFIG_H_
#define INCLUDE_CONFIG_H_

#include <string>
#include <mutex>

#include <nlohmann/json.hpp>

namespace interview::common {


// "X-Api-App-ID":
// "X-Api-Access-Key": 
// "X-Api-Resource-Id": 
// "X-Api-App-Key": 
// "X-Api-Connect-Id": 
struct WsHeadersConfig {
    std::string api_app_id;
    std::string api_access_key;
    std::string api_resource_id;
    std::string api_app_key;
    std::string api_connect_id;
};

// WebSocket 相关配置。
// base_url 表示服务地址；headers 存放握手时需要注入的业务头。
struct WsConfig {
    // WebSocket 服务地址。
    std::string base_url;
    WsHeadersConfig headers;
};

struct LLMConfig {
    // 完整请求地址。
    // 例如：https://api.tokenpony.cn/v1/chat/completions
    std::string api_url;

    // API Key。
    std::string api_key;

    // 模型名。
    // 例如：qwen3-8b
    std::string model;

    // 采样温度。
    double temperature = 0.3;

    // 最大 token 数。
    int max_tokens = 32000;

    // 超时时间，单位秒。
    int timeout_seconds = 60;
};



// 实时对话中的 dialog 配置。
// 这些字段后续会由 Config::BuildStartSessionPayload() 统一组装成
// StartSession 请求体，避免把业务配置拼装逻辑散落到网络层里。
struct DialogConfig {
    // "dialog": {
    //     "bot_name": "豆包",
    //     "system_role": "你使用活泼灵动的女声，性格开朗，热爱生活。",
    //     "speaking_style": "你的说话风格简洁明了，语速适中，语调自然。",
    //     # "character_manifest": "外貌与穿着\n26岁，短发干净利落，眉眼分明，笑起来露出整齐有力的牙齿。体态挺拔，肌肉线条不夸张但明显。常穿简单的衬衫或夹克，看似随意，但每件衣服都干净整洁，给人一种干练可靠的感觉。平时冷峻，眼神锐利，专注时让人不自觉紧张。\n\n性格特点\n平时话不多，不喜欢多说废话，通常用“嗯”或者短句带过。但内心极为细腻，特别在意身边人的感受，只是不轻易表露。嘴硬是常态，“少管我”是他的常用台词，但会悄悄做些体贴的事情，比如把对方喜欢的饮料放在手边。战斗或训练后常说“没事”，但动作中透露出疲惫，习惯用小动作缓解身体酸痛。\n性格上坚毅果断，但不会冲动，做事有条理且有原则。\n\n常用表达方式与口头禅\n\t•\t认可对方时：\n“行吧，这次算你靠谱。”（声音稳重，手却不自觉放松一下，心里松口气）\n\t•\t关心对方时：\n“快点回去，别磨蹭。”（语气干脆，但眼神一直追着对方的背影）\n\t•\t想了解情况时：\n“刚刚……你看到那道光了吗？”（话语随意，手指敲着桌面，但内心紧张，小心隐藏身份）",
    //     "location": {
    //       "city": "北京",
    //     },
    //     "extra": {
    //         "strict_audit": False,
    //         "audit_response": "支持客户自定义安全审核回复话术。",
    //         "recv_timeout": 10,
    //         "input_mod": "audio"
    //     }
    // }
    std::string bot_name = "AI面试官";                            // AI名称
    std::string system_role = "你是一名严格但友好的技术面试官";     // 定义AI的核心行为和禁止事项
    std::string speaking_style = "professional";                 // 定义AI的表达方式和语气
    std::string city = "beijing";                                // 城市信息，可能影响口音或本地化内容
    bool strict_audio = false;                                   // 是否启用严格内容审核
    std::string audit_response = "抱歉，这个问题我不能回答。";     // 审核触发时的回复话术
    int recv_timeout = 5000;                                     // 接收超时时间（秒）
    std::string input_mod = "audio";                             // 输入模式："audio"=语音输入，"text"=文本输入
};

// ASR相关配置
// 这些参数目前主要用于 StartSession payload中的 asr.extra
struct AsrConfig {
    int end_smooth_window_ms = 2000;         // 结束平滑窗口（毫秒），平滑处理语音结束检测
    int vad_silence_duration = 2500;         // VAD静音持续时间（毫秒）
    int vad_speech_trigger_duration = 400;  // VAD语音触发持续时间（毫秒）
};

// TTS 相关配置。
// speaker 和 audio_config 后续会一起进入 StartSession payload。
struct TtsConfig {
    std::string speaker = "zh_male_yunzhou_jupiter_bigtts";
    int channel = 1;
    std::string format = "pcm";
    int sample_rate = 24000;
};

// 通用音频配置。
// 这里同时服务于 audio_input 和 audio_output。
// 例如：
// - audio_input: 录音上传参数
// - audio_output: 播放输出参数
struct AudioConfig {
    int chunk = 3200; // input default: 200 ms at 16 kHz mono PCM.
    int channels = 1;
    int sample_rate = 16000;
    int bit_size = 16;
    std::string format = "pcm";
};

// 项目整体配置快照。
// 这个结构体仍然保留，是为了兼容旧代码中“直接 LoadConfig 返回一份配置副本”
// 的使用方式，避免你现在一次性改太多调用点。
struct AppConfig {
    WsConfig ws;
    LLMConfig llm;
    DialogConfig dialog;
    AsrConfig asr;
    TtsConfig tts;
    AudioConfig audio_input{};
    AudioConfig audio_output{4800, 1, 24000, 16, "pcm"};
};

// Config 单例。
// Stage 7 的目标不是只做“读 JSON”，而是让配置变成项目里的统一数据源：
// 1. main 入口统一加载
// 2. RealLLMClient / RealRealtimeClient 统一读取
// 3. BuildStartSessionPayload() 统一组装实时会话启动参数
//
// 这里不做多余设计，只按你给的方案保留最小必需接口。
class Config {
public:
    // 获取全局唯一配置实例。
    static Config& Instance();
    
    Config(const Config&) = delete;
    Config& operator=(const Config&) = delete;

    // 从文件加载配置。
    // 返回 true 表示加载成功，false 表示打开文件失败或 JSON 解析失败。
    bool LoadFromFile(const std::string& file_path);

    // 根据当前配置生成 StartSession 的 payload。
    // 这样网络层只负责“发送什么事件”，不负责“业务 JSON 怎么拼”。
    nlohmann::json BuildStartSessionPayload() const;

    // 导出一份当前配置快照。
    // 主要用于兼容旧代码路径：LoadConfig(file_path) -> AppConfig。
    AppConfig Snapshot() const;

private:
    Config() = default;

    mutable std::mutex mutex_;
    AppConfig config_;
};
// 从 JSON 文件加载配置。
// 参数：
//   file_path: 配置文件路径
//
// 返回值：
//   解析后的 AppConfig
//
// 如果读取失败或字段缺失，当前版本会抛出异常。
// 后面如果你想做得更工程化，可以再改成返回 bool + 错误信息。
AppConfig LoadConfig(const std::string& file_path);

}  // namespace interview::common

#endif
