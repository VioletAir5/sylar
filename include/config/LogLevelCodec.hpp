#pragma once

#include <stdexcept>
#include <string>

#include "config/YamlCodec.hpp"
#include "log/Level.hpp"

namespace sylar
{

template <> struct LexicalCast<std::string, log::LogLevel>
{
    log::LogLevel operator()(const std::string& value) const
    {
        const auto parsed = log::parseLevel(value);
        if (!parsed)
        {
            throw std::invalid_argument(
                "invalid log level: " + value
            );
        }
        return *parsed;
    }
};

template <> struct LexicalCast<log::LogLevel, std::string>
{
    std::string operator()(log::LogLevel value) const
    {
        return std::string(log::toString(value));
    }
};

} // namespace sylar