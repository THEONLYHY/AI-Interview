#include "common/config.h"

#include <fstream>
#include <stdexcept>

#include <nlohmann/json.hpp>

namespace interview::common {

AppConfig LoadConfig(const std::string& file_path) {

    std::ifstream ifs(file_path);
    if (!ifs.is_open()) {
        throw std::runtime_error("failed to open config file: " + file_path);
    }

    // 解析整个 JSON。
    nlohmann::json root;
    ifs >> root;

    AppConfig config;

    if (root.contains("ws")) {
        const nlohmann::json& ws = root["ws"];

        config.ws.base_url = ws.value("base_url", "");

        if (ws.contains("headers")) {
            const nlohmann::json& headers = ws["headers"];
            config.ws.api_app_id = headers.value("X-Api-App-ID", "");
            config.ws.api_access_key = headers.value("X-Api-Access-Key", "");
            config.ws.api_resource_id = headers.value("X-Api-Resource-Id", "");
            config.ws.api_app_key = headers.value("X-Api-App-Key", "");
            config.ws.api_connect_id = headers.value("X-Api-Connect-Id", "");
        }
    }

    // 解析 llm 配置。
    if (root.contains("llm")) {
        const nlohmann::json& llm = root["llm"];

        config.llm.api_url = llm.value("api_url", "");
        config.llm.api_key = llm.value("api_key", "");
        config.llm.model = llm.value("model", "");
        config.llm.temperature = llm.value("temperature", 0.3);
        config.llm.max_tokens = llm.value("max_tokens", 32000);
        config.llm.timeout_seconds = llm.value("timeout_seconds", 60);
    }
    return config;
}

}  // namespace interview::common
