#pragma once

#include <optional>
#include <string_view>

namespace sylar::log
{

enum class LogLevel
{
    DEBUG,
    INFO,
    WARN,
    ERROR,
    FATAL,
    OFF
};

std::string_view toString(LogLevel level) noexcept;
std::optional<LogLevel> parseLevel(std::string_view name) noexcept;

} // namespace sylar::log
