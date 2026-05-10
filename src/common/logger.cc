#include "common/logger.h"

#include <memory>
#include <vector>

#include <spdlog/pattern_formatter.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

namespace interview::common {

std::shared_ptr<spdlog::logger> Logger::logger_ = nullptr;

bool Logger::Init() {
    if (logger_ != nullptr) {
        return true;
    }

    try {
        // 创建控制台日志
        auto console_sink = std::make_shared<spdlog::sinks::ansicolor_stdout_sink_mt>();
        // 创建文件日志
        auto file_sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>("ai_interview.log", true);

        // 设置日志level
        console_sink->set_level(spdlog::level::info);
        file_sink->set_level(spdlog::level::trace);
        // 日志格式
        console_sink->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] %v");
        file_sink->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%l] [thread %t] %v");

        std::vector<spdlog::sink_ptr> sinks{console_sink, file_sink};

        logger_ = std::make_shared<spdlog::logger>("ai_interview", sinks.begin(), sinks.end());
        logger_->set_level(spdlog::level::trace);
        logger_->flush_on(spdlog::level::warn);
        // 把logger注册到spdlog的全局管理器
        spdlog::register_logger(logger_);
        spdlog::set_default_logger(logger_);

        return true;
    } catch (const spdlog::spdlog_ex& ex) {
        return false;
    }
}

std::shared_ptr<spdlog::logger> Logger::Get() {
    return logger_;
}

}  // namespace interview::common
