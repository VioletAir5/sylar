#pragma once

#include <chrono>
#include <cstddef>
#include <span>

#include "net/Socket.hpp"
#include "net/Stream.hpp"

#include "fiber/IOManager.hpp"

namespace sylar
{

class SocketStream final : public Stream
{
public:
    SocketStream(
        IOManager& io,
        Socket socket,
        std::chrono::milliseconds timeout = std::chrono::seconds(30)
    );

    std::size_t readSome(std::span<std::byte> buffer) override;
    std::size_t writeSome(std::span<const std::byte> buffer) override;

    Socket& socket() noexcept;
    const Socket& socket() const noexcept;

private:
    IOManager& m_io;
    Socket m_socket;
    std::chrono::milliseconds m_timeout;
};

} // namespace sylar
