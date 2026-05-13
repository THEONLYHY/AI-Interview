#ifndef INCLUDE_CONFIG_H_
#define INCLUDE_CONFIG_H_

#include <string>

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
    std::string bot_name = "AI面试官";
    std::string system_role = "你是一名严格但友好的技术面试官";
    std::string speaking_style = "professional";
    std::string city = "wuhan";
    bool strict_audio = false;
    std::string audit_response = "抱歉，这个问题我不能回答。";
    int recv_timeout = 5000;
    std::string input_mod = "audio";
};


// 项目总配置。
// 把 ws 和 llm 两部分统一收口。
struct AppConfig {
    WsConfig ws;
    LLMConfig llm;
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
