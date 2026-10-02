#include "net/Stream.hpp"

#include <stdexcept>
#include <utility>

namespace sylar
{

void Stream::setRequestContext(std::shared_ptr<RequestContext> context
)
{
    m_requestContext = std::move(context);
}

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
