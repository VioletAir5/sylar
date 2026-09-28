#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace sylar
{

struct Uri
{
    std::string scheme;
    std::string host;
    std::uint16_t port = 0;
    std::string target;

    static Uri parse(std::string_view text);
    std::string authority() const;
};

} // namespace sylar
