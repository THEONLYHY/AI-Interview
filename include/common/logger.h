#ifndef INCLUDE_LOGGER_H_
#define INCLUDE_LOGGER_H_

#include <memory>

#include <spdlog/logger.h>

namespace interview::common {

class Logger {
 public:
  static bool Init();
  static std::shared_ptr<spdlog::logger> Get();

 private:
  static std::shared_ptr<spdlog::logger> logger_;
};

}  // namespace interview::common


// LOG_INFO("server started");
// LOG_INFO("client connected, fd = {}", fd);
// LOG_ERROR("recv failed, fd = {}, error = {}", fd, strerror(errno));
//
// 宏体写全限定名，保证任意 namespace 内调用都不会找错符号。
#define LOG_TRACE(...) ::interview::common::Logger::Get()->trace(__VA_ARGS__)
#define LOG_DEBUG(...) ::interview::common::Logger::Get()->debug(__VA_ARGS__)
#define LOG_INFO(...) ::interview::common::Logger::Get()->info(__VA_ARGS__)
#define LOG_WARN(...) ::interview::common::Logger::Get()->warn(__VA_ARGS__)
#define LOG_ERROR(...) ::interview::common::Logger::Get()->error(__VA_ARGS__)
#define LOG_CRITICAL(...) ::interview::common::Logger::Get()->critical(__VA_ARGS__)

#endif  // INCLUDE_LOGGER_H_
