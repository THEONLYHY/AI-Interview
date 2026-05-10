#ifndef INCLUDE_CONFIG_H_
#define INCLUDE_CONFIG_H_

#include <string>

namespace interview::common {

// WebSocket 相关配置。
// 第四阶段以后接实时语音链路时会用到。
struct WsConfig {
    // WebSocket 服务地址。
    std::string base_url;

    // 请求头中的 App ID。
    std::string api_app_id;

    // 请求头中的 Access Key。
    std::string api_access_key;

    // 请求头中的 Resource ID。
    std::string api_resource_id;

    // 请求头中的 App Key。
    std::string api_app_key;

    // 请求头中的 Connect ID。
    // 如果配置文件里为空，后续也可以在运行时自动生成。
    std::string api_connect_id;
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
