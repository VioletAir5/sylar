#include "net/Socket.hpp"

#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <unistd.h>

#include <cerrno>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace sylar
{
namespace
{

void checkFd(int fd)
{
    if (fd < 0)
    {
        throw std::logic_error("socket is closed");
    }
}

void setFlag(int fd, int level, int option, bool enabled)
{
    const int value = enabled ? 1 : 0;
    if (::setsockopt(fd, level, option, &value, sizeof(value)) < 0)
    {
        throw std::system_error(
            errno,
            std::generic_category(),
            "setsockopt"
        );
    }
}

} // namespace

Socket::Socket(int fd, bool owned) noexcept
    : m_fd(fd)
    , m_owned(owned)
{
}

Socket Socket::tcp(int family)
{
    const int fd = ::socket(
        family,
        SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC,
        0
    );
    if (fd < 0)
    {
        throw std::system_error(
            errno,
            std::generic_category(),
            "socket"
        );
    }
    return Socket(fd);
}

Socket Socket::udp(int family)
{
    const int fd = ::socket(
        family,
        SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC,
        0
    );
    if (fd < 0)
    {
        throw std::system_error(
            errno,
            std::generic_category(),
            "socket"
        );
    }
    return Socket(fd);
}

Socket Socket::adopt(int fd)
{
    if (fd < 0)
    {
        throw std::invalid_argument("invalid socket descriptor");
    }
    const int flags = ::fcntl(fd, F_GETFL);
    if (flags < 0 || (flags & O_NONBLOCK) == 0)
    {
        throw std::invalid_argument(
            "adopt requires nonblocking socket"
        );
    }
    const int fd_flags = ::fcntl(fd, F_GETFD);
    if (fd_flags < 0 || (fd_flags & FD_CLOEXEC) == 0)
    {
        throw std::invalid_argument(
            "adopt requires close-on-exec socket"
        );
    }
    return Socket(fd);
}

Socket Socket::borrow(int fd)
{
    if (fd < 0)
    {
        throw std::invalid_argument("invalid socket descriptor");
    }
    return Socket(fd, false);
}

Socket::~Socket()
{
    close();
}

Socket::Socket(Socket&& other) noexcept
    : m_fd(std::exchange(other.m_fd, -1))
    , m_owned(std::exchange(other.m_owned, false))
{
}

Socket& Socket::operator=(Socket&& other) noexcept
{
    if (this != &other)
    {
        close();
        m_fd = std::exchange(other.m_fd, -1);
        m_owned = std::exchange(other.m_owned, false);
    }
    return *this;
}

int Socket::fd() const noexcept
{
    return m_fd;
}

bool Socket::valid() const noexcept
{
    return m_fd >= 0;
}

void Socket::close() noexcept
{
    if (m_fd >= 0 && m_owned)
    {
        ::close(m_fd);
    }
    m_fd = -1;
}

void Socket::bind(const Address& address)
{
    checkFd(m_fd);
    if (::bind(m_fd, address.data(), address.size()) < 0)
    {
        throw std::system_error(
            errno,
            std::generic_category(),
            "bind"
        );
    }
}

void Socket::listen(int backlog)
{
    checkFd(m_fd);
    if (::listen(m_fd, backlog) < 0)
    {
        throw std::system_error(
            errno,
            std::generic_category(),
            "listen"
        );
    }
}

std::optional<Socket> Socket::accept()
{
    checkFd(m_fd);
    const int client = ::accept4(
        m_fd,
        nullptr,
        nullptr,
        SOCK_NONBLOCK | SOCK_CLOEXEC
    );
    if (client >= 0)
    {
        return Socket(client);
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR ||
        errno == ECONNABORTED)
    {
        return std::nullopt;
    }
    throw std::system_error(
        errno,
        std::generic_category(),
        "accept4"
    );
}

void Socket::connect(
    IOManager& io,
    const Address& address,
    std::chrono::milliseconds timeout
)
{
    checkFd(m_fd);
    if (timeout.count() < 0)
    {
        throw std::invalid_argument("negative connect timeout");
    }
    if (::connect(m_fd, address.data(), address.size()) == 0)
    {
        return;
    }
    if (errno != EINPROGRESS)
    {
        throw std::system_error(
            errno,
            std::generic_category(),
            "connect"
        );
    }
    const auto result = io.waitWritable(m_fd, timeout);
    if (result == IOManager::WaitResult::TimedOut)
    {
        throw std::system_error(
            ETIMEDOUT,
            std::generic_category(),
            "connect"
        );
    }
    if (result == IOManager::WaitResult::Cancelled)
    {
        throw std::system_error(
            ECANCELED,
            std::generic_category(),
            "connect"
        );
    }
    int error = 0;
    socklen_t length = sizeof(error);
    if (::getsockopt(m_fd, SOL_SOCKET, SO_ERROR, &error, &length) < 0)
    {
        throw std::system_error(
            errno,
            std::generic_category(),
            "getsockopt"
        );
    }
    if (error != 0)
    {
        throw std::system_error(
            error,
            std::generic_category(),
            "connect"
        );
    }
}

Address Socket::localAddress() const
{
    checkFd(m_fd);
    sockaddr_storage value{};
    socklen_t length = sizeof(value);
    if (::getsockname(
            m_fd,
            reinterpret_cast<sockaddr*>(&value),
            &length
        ) < 0)
    {
        throw std::system_error(
            errno,
            std::generic_category(),
            "getsockname"
        );
    }
    return Address(reinterpret_cast<const sockaddr*>(&value), length);
}

Address Socket::peerAddress() const
{
    checkFd(m_fd);
    sockaddr_storage value{};
    socklen_t length = sizeof(value);
    if (::getpeername(
            m_fd,
            reinterpret_cast<sockaddr*>(&value),
            &length
        ) < 0)
    {
        throw std::system_error(
            errno,
            std::generic_category(),
            "getpeername"
        );
    }
    return Address(reinterpret_cast<const sockaddr*>(&value), length);
}

void Socket::setReuseAddress(bool enabled)
{
    checkFd(m_fd);
    setFlag(m_fd, SOL_SOCKET, SO_REUSEADDR, enabled);
}

void Socket::setNoDelay(bool enabled)
{
    checkFd(m_fd);
    setFlag(m_fd, IPPROTO_TCP, TCP_NODELAY, enabled);
}

ssize_t Socket::recvSome(std::span<std::byte> buffer)
{
    checkFd(m_fd);
    return ::recv(m_fd, buffer.data(), buffer.size(), 0);
}

ssize_t Socket::sendSome(std::span<const std::byte> buffer)
{
    checkFd(m_fd);
    return ::send(m_fd, buffer.data(), buffer.size(), MSG_NOSIGNAL);
}

ssize_t Socket::sendTo(
    std::span<const std::byte> buffer, const Address& destination
)
{
    checkFd(m_fd);
    return ::sendto(
        m_fd,
        buffer.data(),
        buffer.size(),
        MSG_NOSIGNAL,
        destination.data(),
        destination.size()
    );
}

std::optional<Socket::Datagram>
Socket::recvFrom(std::span<std::byte> buffer)
{
    checkFd(m_fd);
    sockaddr_storage source{};
    socklen_t length = sizeof(source);
    const auto count = ::recvfrom(
        m_fd,
        buffer.data(),
        buffer.size(),
        0,
        reinterpret_cast<sockaddr*>(&source),
        &length
    );
    if (count < 0)
    {
        if (errno == EAGAIN || errno == EWOULDBLOCK)
        {
            return std::nullopt;
        }
        throw std::system_error(
            errno,
            std::generic_category(),
            "recvfrom"
        );
    }
    return Datagram{
        count,
        Address(reinterpret_cast<const sockaddr*>(&source), length)};
}

} // namespace sylar
