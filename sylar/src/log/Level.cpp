#include "log/Level.hpp"

#include <array>
#include <cctype>

namespace sylar::log
{

std::string_view toString(LogLevel level) noexcept
{
    switch (level)
    {
    case LogLevel::DEBUG:
        return "DEBUG";
    case LogLevel::INFO:
        return "INFO";
    case LogLevel::WARN:
        return "WARN";
    case LogLevel::ERROR:
        return "ERROR";
    case LogLevel::FATAL:
        return "FATAL";
    case LogLevel::OFF:
        return "OFF";
    }
    return "UNKNOWN";
}

std::optional<LogLevel> parseLevel(std::string_view name) noexcept
{
    constexpr std::array levels{
        LogLevel::DEBUG,
        LogLevel::INFO,
        LogLevel::WARN,
        LogLevel::ERROR,
        LogLevel::FATAL,
        LogLevel::OFF
    };

    for (const auto level : levels)
    {
        const auto text = toString(level);
        if (name.size() != text.size())
        {
            continue;
        }
        bool matches = true;
        for (std::size_t index = 0; index < text.size(); ++index)
        {
            if (std::toupper(static_cast<unsigned char>(name[index])
                ) != text[index])
            {
                matches = false;
                break;
            }
        }
        if (matches)
        {
            return level;
        }
    }
    return std::nullopt;
}

} // namespace sylar::log
