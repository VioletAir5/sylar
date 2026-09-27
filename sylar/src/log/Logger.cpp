#include "log/Logger.hpp"

#include <chrono>
#include <exception>
#include <stdexcept>
#include <thread>
#include <utility>

#include "log/PatternFormatter.hpp"

namespace sylar::log
{

Logger::Logger(
    std::string name,
    std::vector<std::shared_ptr<Sink>> sinks,
    LogLevel level
)
    : m_name(std::move(name))
    , m_sinks(std::move(sinks))
    , m_level(level)
{
    for (const auto& sink : m_sinks)
    {
        if (!sink)
        {
            throw std::invalid_argument("Logger sink must not be null"
            );
        }
    }
}

LogLevel Logger::level() const noexcept
{
    return m_level.load(std::memory_order_relaxed);
}

void Logger::setLevel(LogLevel level) noexcept
{
    m_level.store(level, std::memory_order_relaxed);
}

bool Logger::shouldLog(LogLevel level) const noexcept
{
    return level != LogLevel::OFF && level >= this->level();
}

void Logger::log(
    LogLevel level,
    std::string_view message,
    std::source_location source
)
{
    if (!shouldLog(level))
    {
        return;
    }

    LogRecord record{
        level,
        m_name,
        std::chrono::system_clock::now(),
        std::this_thread::get_id(),
        source,
        std::string(message)};

    std::exception_ptr firstError;
    for (const auto& sink : m_sinks)
    {
        try
        {
            sink->log(record);
        }
        catch (...)
        {
            if (!firstError)
            {
                firstError = std::current_exception();
            }
        }
    }

    if (level >= m_flushLevel.load(std::memory_order_relaxed))
    {
        try
        {
            flush();
        }
        catch (...)
        {
            if (!firstError)
            {
                firstError = std::current_exception();
            }
        }
    }

    if (firstError)
    {
        std::rethrow_exception(firstError);
    }
}

void Logger::debug(
    std::string_view message, std::source_location source
)
{
    log(LogLevel::DEBUG, message, source);
}

void Logger::info(
    std::string_view message, std::source_location source
)
{
    log(LogLevel::INFO, message, source);
}

void Logger::warn(
    std::string_view message, std::source_location source
)
{
    log(LogLevel::WARN, message, source);
}

void Logger::error(
    std::string_view message, std::source_location source
)
{
    log(LogLevel::ERROR, message, source);
}

void Logger::fatal(
    std::string_view message, std::source_location source
)
{
    log(LogLevel::FATAL, message, source);
}

void Logger::setPattern(const std::string& pattern)
{
    // Validate the whole pattern before changing any sink.
    PatternFormatter validation(pattern);
    for (const auto& sink : m_sinks)
    {
        sink->setPattern(pattern);
    }
}

void Logger::flushOn(LogLevel level) noexcept
{
    m_flushLevel.store(level, std::memory_order_relaxed);
}

void Logger::flush()
{
    std::exception_ptr firstError;
    for (const auto& sink : m_sinks)
    {
        try
        {
            sink->flush();
        }
        catch (...)
        {
            if (!firstError)
            {
                firstError = std::current_exception();
            }
        }
    }
    if (firstError)
    {
        std::rethrow_exception(firstError);
    }
}

} // namespace sylar::log
