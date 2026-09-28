#include <sys/socket.h>

#include <array>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string_view>

#include "net/TcpServer.hpp"

using namespace std::chrono_literals;

namespace
{

void echoSession(sylar::IOManager& io, int fd)
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
            if (io.waitReadable(fd, 30s) !=
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
                if (io.waitWritable(fd, 30s) ==
                    sylar::IOManager::WaitResult::Ready)
                {
                    continue;
                }
            }
            return;
        }
    }
}

std::uint16_t parsePort(std::string_view text)
{
    unsigned int value = 0;
    const auto [end, error] = std::from_chars(
        text.data(),
        text.data() + text.size(),
        value
    );
    if (error != std::errc{} || end != text.data() + text.size() ||
        value == 0 || value > 65535)
    {
        throw std::invalid_argument("port must be 1..65535");
    }
    return static_cast<std::uint16_t>(value);
}

} // namespace

int main(int argc, char** argv)
{
    try
    {
        const auto port = argc == 2 ? parsePort(argv[1])
                                    : std::uint16_t{9000};
        sylar::IOManager io;
        sylar::TcpServer server(io, echoSession);
        server.start("0.0.0.0", port);
        std::cout << "echo_server listening on port "
                  << server.localPort() << std::endl;
        io.run();
    }
    catch (const std::exception& error)
    {
        std::cerr << "echo_server: " << error.what() << std::endl;
        return 1;
    }
}
