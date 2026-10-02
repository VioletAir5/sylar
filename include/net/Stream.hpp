#pragma once

#include <cstddef>
#include <memory>
#include <span>

namespace sylar
{

class RequestContext;

class Stream
{
public:
    virtual ~Stream() = default;
    virtual std::size_t readSome(std::span<std::byte> buffer) = 0;
    virtual std::size_t writeSome(std::span<const std::byte> buffer
    ) = 0;

    void readExact(std::span<std::byte> buffer);
    void writeAll(std::span<const std::byte> buffer);
    void setRequestContext(std::shared_ptr<RequestContext> context);

protected:
    std::shared_ptr<RequestContext> m_requestContext;
};

} // namespace sylar
