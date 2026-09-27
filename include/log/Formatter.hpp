#pragma once

#include <string>

#include "log/LogRecord.hpp"

namespace sylar::log
{

class Formatter
{
public:
    virtual ~Formatter() = default;
    virtual std::string format(const LogRecord& record) const = 0;
};

} // namespace sylar::log
