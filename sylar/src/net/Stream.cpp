#include "net/Stream.hpp"

#include <stdexcept>

namespace sylar
{

void Stream::readExact(std::span<std::byte> buffer)
{
    while (!buffer.empty())
    {
        const auto count = readSome(buffer);
        if (count == 0 || count > buffer.size())
        {
            throw std::runtime_error("unexpected end of stream");
        }
        buffer = buffer.subspan(count);
    }
}

void Stream::writeAll(std::span<const std::byte> buffer)
{
    while (!buffer.empty())
    {
        const auto count = writeSome(buffer);
        if (count == 0 || count > buffer.size())
        {
            throw std::runtime_error("stream write failed");
        }
        buffer = buffer.subspan(count);
    }
}

} // namespace sylar
