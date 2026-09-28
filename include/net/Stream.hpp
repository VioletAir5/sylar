#pragma once

#include <cstddef>
#include <span>

namespace sylar
{

class Stream
{
public:
    virtual ~Stream() = default;
    virtual std::size_t readSome(std::span<std::byte> buffer) = 0;
    virtual std::size_t writeSome(std::span<const std::byte> buffer
    ) = 0;

    void readExact(std::span<std::byte> buffer);
    void writeAll(std::span<const std::byte> buffer);
};

} // namespace sylar
