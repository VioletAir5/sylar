#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>

#include "log/Formatter.hpp"
#include "log/Level.hpp"
#include "log/LogRecord.hpp"

namespace sylar::log
{

class Sink
{
public:
    Sink();
    virtual ~Sink() = default;

    Sink(const Sink&) = delete;
    Sink& operator=(const Sink&) = delete;

    void setLevel(LogLevel level) noexcept;
    LogLevel level() const noexcept;
    void setFormatter(std::unique_ptr<Formatter> formatter);
    void setPattern(std::string pattern);

    void log(const LogRecord& record);
    void flush();

protected:
    // The base class holds this lock while formatting, writing and
    // flushing.
    std::mutex m_mutex;

    virtual void writeFormatted(std::string_view line) = 0;
    virtual void flushOutput() = 0;

private:
    std::atomic<LogLevel> m_level{LogLevel::DEBUG};
    std::unique_ptr<Formatter> m_formatter;
};

} // namespace sylar::log
