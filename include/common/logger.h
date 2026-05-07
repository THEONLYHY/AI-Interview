#ifndef INCLUDE_LOGGER_H_
#define INCLUDE_LOGGER_H_

#include <memory>

#include <spdlog/logger.h>

class Logger {
 public:
  static bool Init();
  static std::shared_ptr<spdlog::logger> Get();

 private:
  static std::shared_ptr<spdlog::logger> logger_;
};


// LOG_INFO("server started");
// LOG_INFO("client connected, fd = {}", fd);
// LOG_ERROR("recv failed, fd = {}, error = {}", fd, strerror(errno));

#define LOG_TRACE(...) Logger::Get()->trace(__VA_ARGS__)
#define LOG_DEBUG(...) Logger::Get()->debug(__VA_ARGS__)
#define LOG_INFO(...) Logger::Get()->info(__VA_ARGS__)
#define LOG_WARN(...) Logger::Get()->warn(__VA_ARGS__)
#define LOG_ERROR(...) Logger::Get()->error(__VA_ARGS__)
#define LOG_CRITICAL(...) Logger::Get()->critical(__VA_ARGS__)

#endif  // INCLUDE_LOGGER_H_

