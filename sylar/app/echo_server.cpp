#include <pthread.h>
#include <signal.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <system_error>

#include "net/TcpServer.hpp"
#include "os/SignalFd.hpp"

#include "config/Config.hpp"
#include "config/LogLevelCodec.hpp"
#include "log/Logger.hpp"
#include "log/OstreamSink.hpp"

using namespace std::chrono_literals;

namespace
{

class IdleDeadline final
    : public std::enable_shared_from_this<IdleDeadline>
{
public:
    IdleDeadline(
        sylar::IOManager& io,
        int fd,
        std::chrono::milliseconds timeout
    )
        : m_io(io)
        , m_fd(fd)
        , m_timeout(timeout)
        , m_lastActivity(Clock::now())
    {
    }

    ~IdleDeadline()
    {
        if (m_timer != 0)
        {
            m_io.cancelTimer(m_timer);
        }
    }

    void start()
    {
        arm(m_timeout);
    }

    void touch() noexcept
    {
        m_lastActivity = Clock::now();
    }

private:
    using Clock = std::chrono::steady_clock;

    void arm(std::chrono::milliseconds delay)
    {
        std::weak_ptr<IdleDeadline> weak = shared_from_this();
        m_timer = m_io.addTimer(
            delay,
            [weak]
            {
                if (auto deadline = weak.lock())
                {
                    deadline->check();
                }
            }
        );
    }

    void check()
    {
        m_timer = 0;
        const auto elapsed = Clock::now() - m_lastActivity;
        if (elapsed >= m_timeout)
        {
            m_io.cancel(m_fd, sylar::IOManager::Event::Read);
            m_io.cancel(m_fd, sylar::IOManager::Event::Write);
            return;
        }
        arm(std::chrono::ceil<std::chrono::milliseconds>(
            m_timeout - elapsed
        ));
    }

    sylar::IOManager& m_io;
    int m_fd;
    std::chrono::milliseconds m_timeout;
    Clock::time_point m_lastActivity;
    sylar::TimerManager::TimerId m_timer = 0;
};

void echoSession(
    sylar::IOManager& io,
    int fd,
    std::chrono::milliseconds idle_timeout
)
{
    auto idle = std::make_shared<IdleDeadline>(io, fd, idle_timeout);
    idle->start();
    std::array<char, 8192> buffer{};
    for (;;)
    {
        const auto
            count = ::recv(fd, buffer.data(), buffer.size(), 0);
        if (count == 0)
        {
            return;
        }
        if (count < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            if (errno != EAGAIN && errno != EWOULDBLOCK)
            {
                return;
            }
            if (io.waitReadable(fd) !=
                sylar::IOManager::WaitResult::Ready)
            {
                return;
            }
            continue;
        }

        idle->touch();
        ssize_t offset = 0;
        while (offset < count)
        {
            const auto sent = ::send(
                fd,
                buffer.data() + offset,
                static_cast<std::size_t>(count - offset),
                MSG_NOSIGNAL
            );
            if (sent > 0)
            {
                offset += sent;
                idle->touch();
                continue;
            }
            if (sent < 0 && errno == EINTR)
            {
                continue;
            }
            if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            {
                if (io.waitWritable(fd) ==
                    sylar::IOManager::WaitResult::Ready)
                {
                    continue;
                }
            }
            return;
        }
    }
}

unsigned int parseNumber(std::string_view text)
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
        throw std::invalid_argument("invalid positive number");
    }
    return value;
}

} // namespace

int main(int argc, char** argv)
{
    try
    {
        if (argc > 3)
        {
            throw std::invalid_argument(
                "usage: echo_server [port] [workers] | --config file"
            );
        }

        auto port_setting = sylar::Config::Lookup<int>(
            "server.port",
            9000
        );
        auto workers_setting = sylar::Config::Lookup<int>(
            "server.workers",
            0
        );
        auto timeout_setting = sylar::Config::Lookup<int>(
            "server.idle_timeout_ms",
            30000
        );
        auto level_setting = sylar::Config::Lookup<
            sylar::log::LogLevel>(
            "log.level",
            sylar::log::LogLevel::INFO
        );

        if (argc == 3 && std::string_view(argv[1]) == "--config")
        {
            sylar::Config::LoadFromYaml(YAML::LoadFile(argv[2]));
        }
        else if (argc >= 2)
        {
            port_setting->setValue(
                static_cast<int>(parseNumber(argv[1]))
            );
            if (argc == 3)
            {
                workers_setting->setValue(
                    static_cast<int>(parseNumber(argv[2]))
                );
            }
        }

        const int port = port_setting->getValue();
        const int worker_count = workers_setting->getValue();
        const int timeout_ms = timeout_setting->getValue();
        if (port < 1 || port > 65535 || worker_count < 0 ||
            worker_count > 128 || timeout_ms < 1 ||
            timeout_ms > 600000)
        {
            throw std::invalid_argument(
                "invalid server port, workers, or timeout"
            );
        }

        sylar::log::Logger logger(
            "echo",
            {std::make_shared<sylar::log::OstreamSink>(std::cout)},
            level_setting->getValue()
        );
        const auto idle_timeout = std::chrono::milliseconds(timeout_ms
        );
        auto handler = [idle_timeout](sylar::IOManager& io, int fd)
        { echoSession(io, fd, idle_timeout); };

        sylar::SignalFd signals;
        sylar::IOManager io;
        std::unique_ptr<sylar::IOManagerPool> workers;
        std::unique_ptr<sylar::TcpServer> server;
        if (worker_count != 0)
        {
            workers = std::make_unique<sylar::IOManagerPool>(
                worker_count
            );
            server = std::make_unique<sylar::TcpServer>(
                io,
                *workers,
                handler
            );
        }
        else
        {
            server = std::make_unique<sylar::TcpServer>(io, handler);
        }
        server->start("0.0.0.0", static_cast<std::uint16_t>(port));
        SYLAR_LOG_INFO(
            logger,
            "echo_server listening on port {} workers={}",
            server->localPort(),
            worker_count
        );
        logger.flush();

        io.schedule(
            [&]
            {
                for (;;)
                {
                    if (io.waitReadable(signals.get()) !=
                        sylar::IOManager::WaitResult::Ready)
                    {
                        return;
                    }
                    signalfd_siginfo info{};
                    if (::read(signals.get(), &info, sizeof(info)) ==
                        sizeof(info))
                    {
                        server->stop();
                        io.requestStop();
                        return;
                    }
                }
            }
        );
        io.runForever();
        server->stop();
        if (workers)
        {
            workers->stop();
        }
        SYLAR_LOG_INFO(logger, "echo_server stopped");
        logger.flush();
    }
    catch (const std::exception& error)
    {
        std::cerr << "echo_server: " << error.what() << std::endl;
        return 1;
    }
}
