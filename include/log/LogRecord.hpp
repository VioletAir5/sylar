#pragma once

#include <chrono>
#include <source_location>
#include <string>
#include <thread>

#include "log/Level.hpp"

namespace sylar::log
{

struct LogRecord
{
    LogLevel m_level;
    std::string m_name;
    std::chrono::system_clock::time_point m_timeStamp;
    std::thread::id m_threadId;
    std::source_location m_source;
    std::string m_message;
};

} // namespace sylar::log
