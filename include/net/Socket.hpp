#pragma once

#include <sys/socket.h>

#include <chrono>
#include <cstddef>
#include <optional>
#include <span>

#include "net/Address.hpp"

#include "fiber/IOManager.hpp"

namespace sylar
{

// Unique owner of a nonblocking, close-on-exec socket.
class Socket
{
public:
    static Socket tcp(int family = AF_INET);
    static Socket udp(int family = AF_INET);
    static Socket adopt(int fd);
    static Socket borrow(int fd);

    ~Socket();
    Socket(Socket&& other) noexcept;
    Socket& operator=(Socket&& other) noexcept;
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;

    int fd() const noexcept;
    bool valid() const noexcept;
    void close() noexcept;
    void bind(const Address& address);
    void listen(int backlog = SOMAXCONN);
    std::optional<Socket> accept();
    void connect(
        IOManager& io,
        const Address& address,
        std::chrono::milliseconds timeout
    );
    Address localAddress() const;
    Address peerAddress() const;
    void setReuseAddress(bool enabled = true);
    void setNoDelay(bool enabled = true);
    ssize_t recvSome(std::span<std::byte> buffer);
    ssize_t sendSome(std::span<const std::byte> buffer);
    ssize_t sendTo(
        std::span<const std::byte> buffer, const Address& destination
    );

    struct Datagram
    {
        ssize_t count;
        Address source;
    };
    std::optional<Datagram> recvFrom(std::span<std::byte> buffer);

private:
    explicit Socket(int fd, bool owned = true) noexcept;

    int m_fd = -1;
    bool m_owned = true;
};

} // namespace sylar
