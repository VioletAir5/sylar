#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <chrono>
#include <string>
#include <system_error>
#include <thread>

#include <gtest/gtest.h>

#include "net/TcpServer.hpp"

using namespace std::chrono_literals;

namespace
{

TEST(TcpServerTest, EchoesTwoLoopbackClientsAndStops)
{
    sylar::IOManager io;
    int sessions = 0;
    sylar::TcpServer server(
        io,
        [&](sylar::IOManager& manager, int fd)
        {
            std::array<char, 16> buffer{};
            const auto
                count = ::recv(fd, buffer.data(), buffer.size(), 0);
            if (count > 0)
            {
                auto sent = ::send(
                    fd,
                    buffer.data(),
                    count,
                    MSG_NOSIGNAL
                );
                if (sent == count)
                {
                    ++sessions;
                }
            }
            (void)manager;
        }
    );
    server.start("127.0.0.1", 0);
    ASSERT_NE(server.localPort(), 0);

    std::array<int, 2> clients{-1, -1};
    const std::array<std::string, 2> messages{"alpha", "bravo"};
    for (std::size_t index = 0; index < clients.size(); ++index)
    {
        clients[index] = ::socket(
            AF_INET,
            SOCK_STREAM | SOCK_CLOEXEC,
            0
        );
        ASSERT_GE(clients[index], 0);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(server.localPort());
        ASSERT_EQ(
            ::inet_pton(AF_INET, "127.0.0.1", &address.sin_addr),
            1
        );
        ASSERT_EQ(
            ::connect(
                clients[index],
                reinterpret_cast<sockaddr*>(&address),
                sizeof(address)
            ),
            0
        );
        ASSERT_EQ(
            ::send(
                clients[index],
                messages[index].data(),
                messages[index].size(),
                MSG_NOSIGNAL
            ),
            static_cast<ssize_t>(messages[index].size())
        );
        ASSERT_EQ(::fcntl(clients[index], F_SETFL, O_NONBLOCK), 0);
    }

    const auto stop_timer = io.addTimer(2s, [&] { server.stop(); });
    std::array<std::string, 2> received;
    for (std::size_t index = 0; index < clients.size(); ++index)
    {
        io.schedule(
            [&, index]
            {
                if (io.waitReadable(clients[index], 1s) ==
                    sylar::IOManager::WaitResult::Ready)
                {
                    std::array<char, 16> buffer{};
                    const auto count = ::recv(
                        clients[index],
                        buffer.data(),
                        buffer.size(),
                        0
                    );
                    if (count > 0)
                    {
                        received[index].assign(
                            buffer.data(),
                            static_cast<std::size_t>(count)
                        );
                    }
                }
                ::close(clients[index]);
                clients[index] = -1;
                if (received[0] == messages[0] &&
                    received[1] == messages[1])
                {
                    io.cancelTimer(stop_timer);
                    server.stop();
                }
            }
        );
    }
    io.run();

    for (int fd : clients)
    {
        if (fd >= 0)
        {
            ::close(fd);
        }
    }
    EXPECT_EQ(received, messages);
    EXPECT_EQ(sessions, 2);
    EXPECT_FALSE(server.running());
}

TEST(TcpServerTest, RejectsInvalidAddress)
{
    sylar::IOManager io;
    sylar::TcpServer server(io, [](sylar::IOManager&, int) {});
    EXPECT_THROW(
        server.start("not-an-address", 1234),
        std::invalid_argument
    );
}

TEST(TcpServerTest, StoppedInstanceCannotRestart)
{
    sylar::IOManager io;
    sylar::TcpServer server(io, [](sylar::IOManager&, int) {});
    server.start("127.0.0.1", 0);
    server.stop();

    EXPECT_THROW(server.start("127.0.0.1", 0), std::logic_error);
    io.run();
}

TEST(TcpServerTest, HandlesConnectionOnWorkerThread)
{
    sylar::IOManager acceptor;
    sylar::IOManagerPool workers(2);
    std::thread::id handler_thread;
    sylar::TcpServer server(
        acceptor,
        workers,
        [&](sylar::IOManager&, int fd)
        {
            handler_thread = std::this_thread::get_id();
            char byte;
            if (::recv(fd, &byte, 1, 0) == 1)
            {
                (void)::send(fd, &byte, 1, MSG_NOSIGNAL);
            }
        }
    );
    server.start("127.0.0.1", 0);

    const int client = ::socket(
        AF_INET,
        SOCK_STREAM | SOCK_CLOEXEC,
        0
    );
    ASSERT_GE(client, 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(server.localPort());
    ASSERT_EQ(
        ::inet_pton(AF_INET, "127.0.0.1", &address.sin_addr),
        1
    );
    ASSERT_EQ(
        ::connect(
            client,
            reinterpret_cast<sockaddr*>(&address),
            sizeof(address)
        ),
        0
    );
    const char sent = 'w';
    ASSERT_EQ(::send(client, &sent, 1, MSG_NOSIGNAL), 1);
    ASSERT_EQ(::fcntl(client, F_SETFL, O_NONBLOCK), 0);

    char received = 0;
    const auto deadline = acceptor.addTimer(
        2s,
        [&] { server.stop(); }
    );
    acceptor.schedule(
        [&]
        {
            if (acceptor.waitReadable(client, 1s) ==
                sylar::IOManager::WaitResult::Ready)
            {
                (void)::recv(client, &received, 1, 0);
            }
            acceptor.cancelTimer(deadline);
            server.stop();
        }
    );
    acceptor.run();
    workers.stop();
    ::close(client);

    EXPECT_EQ(received, sent);
    EXPECT_NE(handler_thread, std::this_thread::get_id());
}

TEST(TcpServerTest, IdleConnectionTimesOutAndCloses)
{
    sylar::IOManager io;
    bool timed_out = false;
    sylar::TcpServer server(
        io,
        [&](sylar::IOManager& manager, int fd)
        {
            timed_out = manager.waitReadable(fd, 1ms) ==
                        sylar::IOManager::WaitResult::TimedOut;
        }
    );
    server.start("127.0.0.1", 0);

    const int client = ::socket(
        AF_INET,
        SOCK_STREAM | SOCK_CLOEXEC,
        0
    );
    ASSERT_GE(client, 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(server.localPort());
    ASSERT_EQ(
        ::inet_pton(AF_INET, "127.0.0.1", &address.sin_addr),
        1
    );
    ASSERT_EQ(
        ::connect(
            client,
            reinterpret_cast<sockaddr*>(&address),
            sizeof(address)
        ),
        0
    );
    io.addTimer(10ms, [&] { server.stop(); });

    io.run();

    char byte;
    EXPECT_TRUE(timed_out);
    EXPECT_EQ(::recv(client, &byte, 1, 0), 0);
    ::close(client);
}

} // namespace
