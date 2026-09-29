#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace
{

using Clock = std::chrono::steady_clock;

class UniqueFd
{
public:
    UniqueFd() = default;
    explicit UniqueFd(int value)
        : m_value(value)
    {
    }
    ~UniqueFd()
    {
        reset();
    }
    UniqueFd(const UniqueFd&) = delete;
    UniqueFd& operator=(const UniqueFd&) = delete;
    UniqueFd(UniqueFd&& other) noexcept
        : m_value(other.release())
    {
    }
    UniqueFd& operator=(UniqueFd&& other) noexcept
    {
        if (this != &other)
        {
            reset(other.release());
        }
        return *this;
    }
    int get() const noexcept
    {
        return m_value;
    }
    void reset(int value = -1) noexcept
    {
        if (m_value >= 0)
        {
            ::close(m_value);
        }
        m_value = value;
    }
    int release() noexcept
    {
        const int value = m_value;
        m_value = -1;
        return value;
    }

private:
    int m_value = -1;
};

struct Connection
{
    UniqueFd socket;
    std::size_t sent = 0;
    std::size_t received = 0;
    std::size_t completed = 0;
    Clock::time_point started;
};

unsigned int parseUnsigned(std::string_view text)
{
    unsigned int value = 0;
    const auto [end, error] = std::from_chars(
        text.data(),
        text.data() + text.size(),
        value
    );
    if (error != std::errc{} || end != text.data() + text.size() ||
        value == 0)
    {
        throw std::invalid_argument("invalid positive argument");
    }
    return value;
}

void checkSyscall(int result, const char* name)
{
    if (result < 0)
    {
        throw std::system_error(errno, std::generic_category(), name);
    }
}

void setInterest(
    int epoll_fd,
    std::size_t index,
    const Connection& connection,
    bool writing
)
{
    epoll_event event{};
    event.events = EPOLLIN | EPOLLRDHUP | (writing ? EPOLLOUT : 0U);
    event.data.u32 = static_cast<std::uint32_t>(index);
    checkSyscall(
        ::epoll_ctl(
            epoll_fd,
            EPOLL_CTL_MOD,
            connection.socket.get(),
            &event
        ),
        "epoll_ctl(MOD)"
    );
}

void sendPending(
    int epoll_fd,
    std::size_t index,
    Connection& connection,
    std::string_view payload
)
{
    while (connection.sent < payload.size())
    {
        const auto count = ::send(
            connection.socket.get(),
            payload.data() + connection.sent,
            payload.size() - connection.sent,
            MSG_NOSIGNAL
        );
        if (count > 0)
        {
            connection.sent += static_cast<std::size_t>(count);
            continue;
        }
        if (count < 0 && errno == EINTR)
        {
            continue;
        }
        if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
        {
            setInterest(epoll_fd, index, connection, true);
            return;
        }
        throw std::system_error(
            errno,
            std::generic_category(),
            "send"
        );
    }
    setInterest(epoll_fd, index, connection, false);
}

bool receivePending(Connection& connection, std::string_view expected)
{
    std::array<char, 8192> buffer{};
    while (connection.received < expected.size())
    {
        const auto requested = std::min(
            buffer.size(),
            expected.size() - connection.received
        );
        const auto count = ::recv(
            connection.socket.get(),
            buffer.data(),
            requested,
            0
        );
        if (count > 0)
        {
            for (ssize_t index = 0; index < count; ++index)
            {
                if (buffer[static_cast<std::size_t>(index)] !=
                    expected
                        [connection.received +
                         static_cast<std::size_t>(index)])
                {
                    throw std::runtime_error("HTTP response mismatch"
                    );
                }
            }
            connection.received += static_cast<std::size_t>(count);
            continue;
        }
        if (count == 0)
        {
            throw std::runtime_error(
                "server closed before full HTTP response"
            );
        }
        if (errno == EINTR)
        {
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK)
        {
            return false;
        }
        throw std::system_error(
            errno,
            std::generic_category(),
            "recv"
        );
    }
    return true;
}

void benchmark(
    unsigned int port, unsigned int clients, unsigned int requests
)
{
    if (port > 65535 || clients > 5000 ||
        static_cast<std::uint64_t>(clients) * requests > 5000000)
    {
        throw std::invalid_argument("benchmark limit exceeded");
    }

    UniqueFd epoll(::epoll_create1(EPOLL_CLOEXEC));
    checkSyscall(epoll.get(), "epoll_create1");
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(static_cast<std::uint16_t>(port));
    if (::inet_pton(AF_INET, "127.0.0.1", &address.sin_addr) != 1)
    {
        throw std::runtime_error("invalid loopback address");
    }
    std::vector<Connection> connections(clients);
    for (std::size_t index = 0; index < clients; ++index)
    {
        auto& connection = connections[index];
        connection.socket.reset(
            ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0)
        );
        checkSyscall(connection.socket.get(), "socket");
        checkSyscall(
            ::connect(
                connection.socket.get(),
                reinterpret_cast<const sockaddr*>(&address),
                sizeof(address)
            ),
            "connect"
        );
        const int no_delay = 1;
        checkSyscall(
            ::setsockopt(
                connection.socket.get(),
                IPPROTO_TCP,
                TCP_NODELAY,
                &no_delay,
                sizeof(no_delay)
            ),
            "setsockopt(TCP_NODELAY)"
        );
        const int flags = ::fcntl(connection.socket.get(), F_GETFL);
        checkSyscall(flags, "fcntl(F_GETFL)");
        checkSyscall(
            ::fcntl(
                connection.socket.get(),
                F_SETFL,
                flags | O_NONBLOCK
            ),
            "fcntl(F_SETFL)"
        );
        epoll_event event{};
        event.events = EPOLLIN | EPOLLRDHUP;
        event.data.u32 = static_cast<std::uint32_t>(index);
        checkSyscall(
            ::epoll_ctl(
                epoll.get(),
                EPOLL_CTL_ADD,
                connection.socket.get(),
                &event
            ),
            "epoll_ctl(ADD)"
        );
    }

    const std::string
        request = "GET /health HTTP/1.1\r\nHost: localhost\r\n\r\n";
    const std::string
        expected = "HTTP/1.1 200 OK\r\nContent-Length: 3\r\n"
                   "Connection: keep-alive\r\n\r\nok\n";
    std::vector<double> latency_us;
    latency_us.reserve(static_cast<std::size_t>(clients) * requests);
    const auto wall_start = Clock::now();
    for (std::size_t index = 0; index < clients; ++index)
    {
        auto& connection = connections[index];
        connection.started = Clock::now();
        sendPending(epoll.get(), index, connection, request);
    }

    std::size_t finished = 0;
    std::array<epoll_event, 256> events{};
    while (finished < clients)
    {
        const int count = ::epoll_wait(
            epoll.get(),
            events.data(),
            static_cast<int>(events.size()),
            10000
        );
        checkSyscall(count, "epoll_wait");
        if (count == 0)
        {
            throw std::runtime_error(
                "benchmark timed out waiting for HTTP response"
            );
        }
        for (int event_index = 0; event_index < count; ++event_index)
        {
            const auto index = events[event_index].data.u32;
            auto& connection = connections[index];
            if (connection.socket.get() < 0)
            {
                continue;
            }
            const auto flags = events[event_index].events;
            if (flags & EPOLLOUT && connection.sent < request.size())
            {
                sendPending(epoll.get(), index, connection, request);
            }
            if (flags & EPOLLIN)
            {
                if (receivePending(connection, expected))
                {
                    const auto now = Clock::now();
                    latency_us.push_back(
                        std::chrono::duration<double, std::micro>(
                            now - connection.started
                        )
                            .count()
                    );
                    ++connection.completed;
                    if (connection.completed == requests)
                    {
                        connection.socket.reset();
                        ++finished;
                    }
                    else
                    {
                        connection.sent = 0;
                        connection.received = 0;
                        connection.started = Clock::now();
                        sendPending(
                            epoll.get(),
                            index,
                            connection,
                            request
                        );
                    }
                }
            }
            if (flags & (EPOLLERR | EPOLLHUP | EPOLLRDHUP) &&
                connection.socket.get() >= 0 &&
                connection.received < expected.size())
            {
                throw std::runtime_error(
                    "connection failed during benchmark"
                );
            }
        }
    }
    const auto wall_end = Clock::now();
    std::sort(latency_us.begin(), latency_us.end());
    const auto total = latency_us.size();
    const auto p50 = latency_us[(total - 1) / 2];
    const auto p99 = latency_us[(99 * total + 99) / 100 - 1];
    const auto elapsed = std::chrono::duration<double>(
                             wall_end - wall_start
    )
                             .count();
    std::cout << "clients=" << clients
              << " requests/client=" << requests << " route=/health"
              << " completed=" << total << '\n'
              << "elapsed=" << elapsed << "s"
              << " throughput=" << total / elapsed << " req/s"
              << " p50=" << p50 << "us"
              << " p99=" << p99 << "us"
              << " max=" << latency_us.back() << "us\n";
}

} // namespace

int main(int argc, char** argv)
{
    try
    {
        if (argc != 4)
        {
            throw std::invalid_argument(
                "usage: sylar_http_bench PORT CLIENTS REQUESTS"
            );
        }
        benchmark(
            parseUnsigned(argv[1]),
            parseUnsigned(argv[2]),
            parseUnsigned(argv[3])
        );
    }
    catch (const std::exception& error)
    {
        std::cerr << "sylar_http_bench: " << error.what() << '\n';
        return 1;
    }
}
