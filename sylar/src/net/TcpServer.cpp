#include "net/TcpServer.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace sylar
{
namespace
{

struct OwnedFd
{
    explicit OwnedFd(int value)
        : fd(value)
    {
    }

    ~OwnedFd()
    {
        if (fd >= 0)
        {
            ::close(fd);
        }
    }

    OwnedFd(const OwnedFd&) = delete;
    OwnedFd& operator=(const OwnedFd&) = delete;

    int fd;
};

} // namespace

struct TcpServer::State
{
    State(
        IOManager& io_ref,
        IOManagerPool* worker_pool,
        Handler connection_handler
    )
        : io(io_ref)
        , workers(worker_pool)
        , handler(std::move(connection_handler))
    {
    }

    IOManager& io;
    IOManagerPool* workers;
    Handler handler;
    int listener = -1;
    std::uint16_t port = 0;
    bool running = false;
    bool started = false;
};

TcpServer::TcpServer(IOManager& io, Handler handler)
    : m_state(std::make_shared<State>(io, nullptr, std::move(handler))
      )
{
    if (!m_state->handler)
    {
        throw std::invalid_argument("TcpServer handler is empty");
    }
}

TcpServer::TcpServer(
    IOManager& acceptor, IOManagerPool& workers, Handler handler
)
    : m_state(std::make_shared<State>(
          acceptor, &workers, std::move(handler)
      ))
{
    if (!m_state->handler)
    {
        throw std::invalid_argument("TcpServer handler is empty");
    }
}

TcpServer::~TcpServer()
{
    stop();
}

void TcpServer::start(const std::string& ipv4, std::uint16_t port)
{
    auto& state = *m_state;
    if (state.started)
    {
        throw std::logic_error("TcpServer cannot restart");
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    if (::inet_pton(AF_INET, ipv4.c_str(), &address.sin_addr) != 1)
    {
        throw std::invalid_argument("invalid IPv4 address");
    }

    OwnedFd listener(::socket(
        AF_INET,
        SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC,
        0
    ));
    if (listener.fd < 0)
    {
        throw std::system_error(
            errno,
            std::generic_category(),
            "socket"
        );
    }
    const int reuse = 1;
    if (::setsockopt(
            listener.fd,
            SOL_SOCKET,
            SO_REUSEADDR,
            &reuse,
            sizeof(reuse)
        ) != 0)
    {
        throw std::system_error(
            errno,
            std::generic_category(),
            "setsockopt"
        );
    }
    if (::bind(
            listener.fd,
            reinterpret_cast<const sockaddr*>(&address),
            sizeof(address)
        ) != 0)
    {
        throw std::system_error(
            errno,
            std::generic_category(),
            "bind"
        );
    }
    if (::listen(listener.fd, SOMAXCONN) != 0)
    {
        throw std::system_error(
            errno,
            std::generic_category(),
            "listen"
        );
    }
    socklen_t length = sizeof(address);
    if (::getsockname(
            listener.fd,
            reinterpret_cast<sockaddr*>(&address),
            &length
        ) != 0)
    {
        throw std::system_error(
            errno,
            std::generic_category(),
            "getsockname"
        );
    }

    state.listener = listener.fd;
    state.port = ntohs(address.sin_port);
    state.running = true;
    try
    {
        state.io.schedule([shared = m_state] { acceptLoop(shared); });
    }
    catch (...)
    {
        state.running = false;
        state.listener = -1;
        state.port = 0;
        throw;
    }
    listener.fd = -1;
    state.started = true;
}

void TcpServer::stop()
{
    auto& state = *m_state;
    if (!state.running)
    {
        return;
    }
    state.running = false;
    state.io.cancel(state.listener, IOManager::Event::Read);
    ::close(state.listener);
    state.listener = -1;
    state.port = 0;
}

std::uint16_t TcpServer::localPort() const
{
    return m_state->port;
}

bool TcpServer::running() const
{
    return m_state->running;
}

void TcpServer::acceptLoop(const std::shared_ptr<State>& state)
{
    while (state->running)
    {
        const int fd = ::accept4(
            state->listener,
            nullptr,
            nullptr,
            SOCK_NONBLOCK | SOCK_CLOEXEC
        );
        if (fd >= 0)
        {
            std::shared_ptr<OwnedFd> client;
            try
            {
                client = std::make_shared<OwnedFd>(fd);
            }
            catch (...)
            {
                ::close(fd);
                throw;
            }
            if (state->workers)
            {
                state->workers->post(
                    [state, client](IOManager& worker)
                    { state->handler(worker, client->fd); }
                );
            }
            else
            {
                state->io.schedule(
                    [state, client]
                    { state->handler(state->io, client->fd); }
                );
            }
            continue;
        }
        if (errno == EINTR || errno == ECONNABORTED)
        {
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK)
        {
            const auto result = state->io.waitReadable(state->listener
            );
            if (result == IOManager::WaitResult::Cancelled &&
                !state->running)
            {
                return;
            }
            continue;
        }
        throw std::system_error(
            errno,
            std::generic_category(),
            "accept4"
        );
    }
}

} // namespace sylar
