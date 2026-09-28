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

#include "config/Config.hpp"
#include "config/LogLevelCodec.hpp"
#include "log/Logger.hpp"
#include "log/OstreamSink.hpp"

using namespace std::chrono_literals;

namespace
{

void echoSession(
    sylar::IOManager& io,
    int fd,
    std::chrono::milliseconds idle_timeout
)
{
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
            if (io.waitReadable(fd, idle_timeout) !=
                sylar::IOManager::WaitResult::Ready)
            {
                return;
            }
            continue;
        }

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
                continue;
            }
            if (sent < 0 && errno == EINTR)
            {
                continue;
            }
            if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            {
                if (io.waitWritable(fd, idle_timeout) ==
                    sylar::IOManager::WaitResult::Ready)
                {
                    continue;
                }
            }
            return;
        }
    }
}

class SignalFd
{
public:
    SignalFd()
    {
        sigset_t mask;
        ::sigemptyset(&mask);
        ::sigaddset(&mask, SIGINT);
        ::sigaddset(&mask, SIGTERM);
        const int result = ::pthread_sigmask(
            SIG_BLOCK,
            &mask,
            nullptr
        );
        if (result != 0)
        {
            throw std::system_error(
                result,
                std::generic_category(),
                "pthread_sigmask"
            );
        }
        m_fd = ::signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC);
        if (m_fd < 0)
        {
            throw std::system_error(
                errno,
                std::generic_category(),
                "signalfd"
            );
        }
    }

    ~SignalFd()
    {
        ::close(m_fd);
    }

    int get() const noexcept
    {
        return m_fd;
    }

private:
    int m_fd = -1;
};

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

        SignalFd signals;
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
