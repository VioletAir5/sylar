#include "log/Sink.hpp"

#include <stdexcept>
#include <utility>

#include "log/PatternFormatter.hpp"

namespace sylar::log
{

Sink::Sink()
    : m_formatter(std::make_unique<PatternFormatter>())
{
}

void Sink::setLevel(LogLevel level) noexcept
{
    m_level.store(level, std::memory_order_relaxed);
}

LogLevel Sink::level() const noexcept
{
    return m_level.load(std::memory_order_relaxed);
}

void Sink::setFormatter(std::unique_ptr<Formatter> formatter)
{
    if (!formatter)
    {
        throw std::invalid_argument("Sink formatter must not be null"
        );
    }
    std::lock_guard lock(m_mutex);
    m_formatter = std::move(formatter);
}

void Sink::setPattern(std::string pattern)
{
    setFormatter(std::make_unique<PatternFormatter>(std::move(pattern)
    ));
}

void Sink::log(const LogRecord& record)
{
    if (record.m_level == LogLevel::OFF || record.m_level < level())
    {
        return;
    }
    std::lock_guard lock(m_mutex);
    auto line = m_formatter->format(record);
    line.push_back('\n');
    writeFormatted(line);
}

void Sink::flush()
{
    std::lock_guard lock(m_mutex);
    flushOutput();
}

} // namespace sylar::log
