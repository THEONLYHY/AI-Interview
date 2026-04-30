#include <iostream>

#include "nlohmann/json.hpp"
#include "spdlog/spdlog.h"

int main() {
    nlohmann::json message;
    message["project"] = "ai_interview";
    message["status"] = "ok";

    spdlog::info("ai_interview start");
    std::cout << message.dump(4) << '\n';

    return 0;
}