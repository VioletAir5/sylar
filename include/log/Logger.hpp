#pragma once

#include <atomic>
#include <memory>
#include <source_location>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <fmt/format.h>

#include "log/Level.hpp"
#include "log/Sink.hpp"

namespace sylar::log
{

class Logger
{
public:
    Logger(
        std::string name,
        std::vector<std::shared_ptr<Sink>> sinks,
        LogLevel level = LogLevel::INFO
    );

    const std::string& name() const noexcept
    {
        return m_name;
    }
    LogLevel level() const noexcept;
    void setLevel(LogLevel level) noexcept;
    bool shouldLog(LogLevel level) const noexcept;

    void
    log(LogLevel level,
        std::string_view message,
        std::source_location source = std::source_location::current()
    );

    template <typename... Args>
    void logFmt(
        LogLevel level,
        std::source_location source,
        fmt::format_string<Args...> format,
        Args&&... args
    )
    {
        if (shouldLog(level))
        {
            log(level,
                fmt::format(format, std::forward<Args>(args)...),
                source);
        }
    }

    void debug(
        std::string_view message,
        std::source_location source = std::source_location::current()
    );
    void info(
        std::string_view message,
        std::source_location source = std::source_location::current()
    );
    void warn(
        std::string_view message,
        std::source_location source = std::source_location::current()
    );
    void error(
        std::string_view message,
        std::source_location source = std::source_location::current()
    );
    void fatal(
        std::string_view message,
        std::source_location source = std::source_location::current()
    );

    void setPattern(const std::string& pattern);
    void flushOn(LogLevel level) noexcept;
    void flush();

private:
    std::string m_name;
    const std::vector<std::shared_ptr<Sink>> m_sinks;
    std::atomic<LogLevel> m_level;
    std::atomic<LogLevel> m_flushLevel{LogLevel::OFF};
};

} // namespace sylar::log

#define SYLAR_LOG(logger, level, ...)                                \
    do                                                               \
    {                                                                \
        auto&& sylar_logger = (logger);                              \
        if (sylar_logger.shouldLog(level))                           \
        {                                                            \
            sylar_logger.logFmt(                                     \
                level,                                               \
                std::source_location::current(),                     \
                __VA_ARGS__                                          \
            );                                                       \
        }                                                            \
    } while (false)

#define SYLAR_LOG_DEBUG(logger, ...)                                 \
    SYLAR_LOG(logger, ::sylar::log::LogLevel::DEBUG, __VA_ARGS__)
#define SYLAR_LOG_INFO(logger, ...)                                  \
    SYLAR_LOG(logger, ::sylar::log::LogLevel::INFO, __VA_ARGS__)
#define SYLAR_LOG_WARN(logger, ...)                                  \
    SYLAR_LOG(logger, ::sylar::log::LogLevel::WARN, __VA_ARGS__)
#define SYLAR_LOG_ERROR(logger, ...)                                 \
    SYLAR_LOG(logger, ::sylar::log::LogLevel::ERROR, __VA_ARGS__)
#define SYLAR_LOG_FATAL(logger, ...)                                 \
    SYLAR_LOG(logger, ::sylar::log::LogLevel::FATAL, __VA_ARGS__)
