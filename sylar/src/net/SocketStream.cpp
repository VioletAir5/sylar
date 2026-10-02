#include "net/SocketStream.hpp"

#include <cerrno>
#include <stdexcept>
#include <system_error>
#include <utility>

#include "fiber/RequestContext.hpp"

namespace sylar
{
namespace
{

void checkWait(IOManager::WaitResult result)
{
    if (result == IOManager::WaitResult::TimedOut)
    {
        throw std::system_error(
            ETIMEDOUT,
            std::generic_category(),
            "socket wait"
        );
    }
    if (result == IOManager::WaitResult::Cancelled)
    {
        throw std::system_error(
            ECANCELED,
            std::generic_category(),
            "socket wait"
        );
    }
}

} // namespace

SocketStream::SocketStream(
    IOManager& io, Socket socket, std::chrono::milliseconds timeout
)
    : m_io(io)
    , m_socket(std::move(socket))
    , m_timeout(timeout)
{
    if (timeout.count() < 0)
    {
        throw std::invalid_argument("negative socket timeout");
    }
}

std::size_t SocketStream::readSome(std::span<std::byte> buffer)
{
    if (buffer.empty())
    {
        return 0;
    }
    for (;;)
    {
        if (m_requestContext && (m_requestContext->expired() ||
                                 m_requestContext->cancelled()))
        {
            throw std::system_error(
                m_requestContext->cancelled() ? ECANCELED : ETIMEDOUT,
                std::generic_category(),
                "socket read"
            );
        }
        const auto count = m_socket.recvSome(buffer);
        if (count >= 0)
        {
            return static_cast<std::size_t>(count);
        }
        if (errno == EINTR)
        {
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK)
        {
            checkWait(
                m_requestContext
                    ? m_io.waitReadable(
                          m_socket.fd(),
                          m_timeout,
                          *m_requestContext
                      )
                    : m_io.waitReadable(m_socket.fd(), m_timeout)
            );
            continue;
        }
        throw std::system_error(
            errno,
            std::generic_category(),
            "recv"
        );
    }
}

std::size_t SocketStream::writeSome(std::span<const std::byte> buffer)
{
    if (buffer.empty())
    {
        return 0;
    }
    for (;;)
    {
        if (m_requestContext && (m_requestContext->expired() ||
                                 m_requestContext->cancelled()))
        {
            throw std::system_error(
                m_requestContext->cancelled() ? ECANCELED : ETIMEDOUT,
                std::generic_category(),
                "socket write"
            );
        }
        const auto count = m_socket.sendSome(buffer);
        if (count >= 0)
        {
            return static_cast<std::size_t>(count);
        }
        if (errno == EINTR)
        {
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK)
        {
            checkWait(
                m_requestContext
                    ? m_io.waitWritable(
                          m_socket.fd(),
                          m_timeout,
                          *m_requestContext
                      )
                    : m_io.waitWritable(m_socket.fd(), m_timeout)
            );
            continue;
        }
        throw std::system_error(
            errno,
            std::generic_category(),
            "send"
        );
    }
}

Socket& SocketStream::socket() noexcept
{
    return m_socket;
}

const Socket& SocketStream::socket() const noexcept
{
    return m_socket;
}

} // namespace sylar
