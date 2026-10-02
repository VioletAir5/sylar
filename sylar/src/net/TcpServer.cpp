#include "net/TcpServer.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <stdexcept>
#include <system_error>
#include <utility>
#include <vector>

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

    OwnedFd(OwnedFd&& other) noexcept
        : fd(std::exchange(other.fd, -1))
    {
    }

    OwnedFd& operator=(OwnedFd&& other) noexcept
    {
        if (this != &other)
        {
            close();
            fd = std::exchange(other.fd, -1);
        }
        return *this;
    }

    void close() noexcept
    {
        if (fd >= 0)
        {
            ::close(std::exchange(fd, -1));
        }
    }

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
    std::vector<OwnedFd> listeners;
    std::vector<Address> addresses;
    std::uint16_t port = 0;
    bool running = false;
    bool started = false;
    std::size_t max_connections = 0;
    std::atomic_size_t active{0};
    std::atomic_size_t rejected{0};
    struct Connection
    {
        Connection(int value, std::shared_ptr<State> owner)
            : fd(value)
            , state(std::move(owner))
        {
        }

        ~Connection()
        {
            fd.close();
            state->active.fetch_sub(1, std::memory_order_relaxed);
        }

        OwnedFd fd;
        std::shared_ptr<State> state;
    };
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
    start(Address::ipv4(ipv4, port));
}

void TcpServer::start(const Address& address)
{
    start(std::vector<Address>{address});
}

void TcpServer::start(const std::vector<Address>& bind_addresses)
{
    auto& state = *m_state;
    if (state.started)
    {
        throw std::logic_error("TcpServer cannot restart");
    }
    if (bind_addresses.empty())
    {
        throw std::invalid_argument(
            "TcpServer needs at least one address"
        );
    }

    std::vector<OwnedFd> listeners;
    std::vector<Address> actual_addresses;
    listeners.reserve(bind_addresses.size());
    actual_addresses.reserve(bind_addresses.size());
    for (const auto& bind_address : bind_addresses)
    {
        OwnedFd listener(::socket(
            bind_address.family(),
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
        if (bind_address.family() != AF_UNIX)
        {
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
        }
        if (::bind(
                listener.fd,
                bind_address.data(),
                bind_address.size()
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
        sockaddr_storage actual{};
        socklen_t length = sizeof(actual);
        if (::getsockname(
                listener.fd,
                reinterpret_cast<sockaddr*>(&actual),
                &length
            ) != 0)
        {
            throw std::system_error(
                errno,
                std::generic_category(),
                "getsockname"
            );
        }
        actual_addresses.emplace_back(
            reinterpret_cast<const sockaddr*>(&actual),
            length
        );
        listeners.push_back(std::move(listener));
    }

    state.port = actual_addresses.front().family() == AF_UNIX
                     ? 0
                     : actual_addresses.front().port();
    state.listeners = std::move(listeners);
    state.addresses = std::move(actual_addresses);
    state.running = true;
    try
    {
        for (const auto& listener : state.listeners)
        {
            state.io.schedule([shared = m_state, fd = listener.fd]
                              { acceptLoop(shared, fd); });
        }
    }
    catch (...)
    {
        state.running = false;
        state.listeners.clear();
        state.addresses.clear();
        state.port = 0;
        throw;
    }
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
    for (auto& listener : state.listeners)
    {
        state.io.cancel(listener.fd, IOManager::Event::Read);
        listener.close();
    }
    state.listeners.clear();
    state.addresses.clear();
    state.port = 0;
}

void TcpServer::setMaxConnections(std::size_t limit)
{
    if (m_state->started)
    {
        throw std::logic_error(
            "connection limit cannot change after start"
        );
    }
    m_state->max_connections = limit;
}

std::size_t TcpServer::maxConnections() const noexcept
{
    return m_state->max_connections;
}

std::size_t TcpServer::activeConnections() const noexcept
{
    return m_state->active.load(std::memory_order_relaxed);
}

std::size_t TcpServer::rejectedConnections() const noexcept
{
    return m_state->rejected.load(std::memory_order_relaxed);
}

std::uint16_t TcpServer::localPort() const
{
    return m_state->port;
}

std::vector<Address> TcpServer::localAddresses() const
{
    return m_state->addresses;
}

bool TcpServer::running() const
{
    return m_state->running;
}

void TcpServer::acceptLoop(
    const std::shared_ptr<State>& state, int listener
)
{
    while (state->running)
    {
        const int fd = ::accept4(
            listener,
            nullptr,
            nullptr,
            SOCK_NONBLOCK | SOCK_CLOEXEC
        );
        if (fd >= 0)
        {
            if (state->max_connections != 0 &&
                state->active.load(std::memory_order_relaxed) >=
                    state->max_connections)
            {
                state->rejected.fetch_add(
                    1,
                    std::memory_order_relaxed
                );
                ::close(fd);
                continue;
            }
            state->active.fetch_add(1, std::memory_order_relaxed);
            std::shared_ptr<State::Connection> client;
            try
            {
                client = std::make_shared<State::Connection>(
                    fd,
                    state
                );
            }
            catch (...)
            {
                state->active.fetch_sub(1, std::memory_order_relaxed);
                ::close(fd);
                throw;
            }
            if (state->workers)
            {
                state->workers->post(
                    [state, client](IOManager& worker)
                    { state->handler(worker, client->fd.fd); }
                );
            }
            else
            {
                state->io.schedule(
                    [state, client]
                    { state->handler(state->io, client->fd.fd); }
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
            const auto result = state->io.waitReadable(listener);
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
