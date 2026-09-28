#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "net/Address.hpp"
#include "net/Socket.hpp"

#include "fiber/Hook.hpp"
#include "fiber/IOManager.hpp"

using namespace std::chrono_literals;

TEST(HookTest, ReadYieldsFiberUntilSocketReady)
{
    int fds[2];
    ASSERT_EQ(
        ::socketpair(
            AF_UNIX,
            SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC,
            0,
            fds
        ),
        0
    );
    sylar::IOManager io;
    std::vector<int> order;
    char received = 0;
    io.schedule(
        [&]
        {
            sylar::HookScope hooks(1s);
            EXPECT_TRUE(sylar::HookScope::enabled());
            EXPECT_EQ(::read(fds[0], &received, 1), 1);
            order.push_back(2);
        }
    );
    io.schedule(
        [&]
        {
            order.push_back(1);
            EXPECT_EQ(::write(fds[1], "x", 1), 1);
        }
    );
    io.run();
    ::close(fds[0]);
    ::close(fds[1]);
    EXPECT_EQ(received, 'x');
    EXPECT_EQ(order, (std::vector<int>{1, 2}));
    EXPECT_FALSE(sylar::HookScope::enabled());
}

TEST(HookTest, SleepYieldsAndReadTimesOut)
{
    int fds[2];
    ASSERT_EQ(
        ::socketpair(
            AF_UNIX,
            SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC,
            0,
            fds
        ),
        0
    );
    sylar::IOManager io;
    std::vector<int> order;
    io.schedule(
        [&]
        {
            sylar::HookScope hooks(5ms);
            EXPECT_EQ(::usleep(1000), 0);
            order.push_back(2);
            char byte;
            EXPECT_EQ(::recv(fds[0], &byte, 1, 0), -1);
            EXPECT_EQ(errno, ETIMEDOUT);
        }
    );
    io.schedule([&] { order.push_back(1); });
    io.run();
    ::close(fds[0]);
    ::close(fds[1]);
    EXPECT_EQ(order, (std::vector<int>{1, 2}));
}

TEST(HookTest, InterleavedScopesKeepHookEnabled)
{
    sylar::IOManager io;
    bool second_still_enabled = false;
    io.schedule(
        [&]
        {
            sylar::HookScope first(1s);
            EXPECT_EQ(::usleep(1000), 0);
        }
    );
    io.schedule(
        [&]
        {
            sylar::HookScope second(1s);
            EXPECT_EQ(::usleep(3000), 0);
            second_still_enabled = sylar::HookScope::enabled();
        }
    );
    io.run();
    EXPECT_TRUE(second_still_enabled);
    EXPECT_FALSE(sylar::HookScope::enabled());
}

TEST(HookTest, ConnectAndAcceptUseFiberWaits)
{
    sylar::IOManager io;
    auto listener = sylar::Socket::tcp();
    listener.bind(sylar::Address::ipv4("127.0.0.1", 0));
    listener.listen();
    const auto address = sylar::Address::ipv4(
        "127.0.0.1",
        listener.localAddress().port()
    );
    char received = 0;
    io.schedule(
        [&]
        {
            sylar::HookScope hooks(1s);
            const int accepted = ::accept(
                listener.fd(),
                nullptr,
                nullptr
            );
            ASSERT_GE(accepted, 0);
            EXPECT_EQ(::read(accepted, &received, 1), 1);
            ::close(accepted);
        }
    );
    io.schedule(
        [&]
        {
            sylar::HookScope hooks(1s);
            auto client = sylar::Socket::tcp();
            ASSERT_EQ(
                ::connect(
                    client.fd(),
                    address.data(),
                    address.size()
                ),
                0
            );
            EXPECT_EQ(::write(client.fd(), "c", 1), 1);
        }
    );
    io.run();
    EXPECT_EQ(received, 'c');
}

TEST(HookTest, UdpReceiveYieldsAndCloseCancelsWait)
{
    sylar::IOManager io;
    auto receiver = sylar::Socket::udp();
    receiver.bind(sylar::Address::ipv4("127.0.0.1", 0));
    auto sender = sylar::Socket::udp();
    char received = 0;
    io.schedule(
        [&]
        {
            sylar::HookScope hooks(1s);
            EXPECT_EQ(
                ::recvfrom(
                    receiver.fd(),
                    &received,
                    1,
                    0,
                    nullptr,
                    nullptr
                ),
                1
            );
        }
    );
    io.schedule(
        [&]
        {
            sylar::HookScope hooks(1s);
            const auto address = receiver.localAddress();
            EXPECT_EQ(
                ::sendto(
                    sender.fd(),
                    "u",
                    1,
                    0,
                    address.data(),
                    address.size()
                ),
                1
            );
        }
    );
    io.run();
    EXPECT_EQ(received, 'u');

    int fds[2];
    ASSERT_EQ(
        ::socketpair(
            AF_UNIX,
            SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC,
            0,
            fds
        ),
        0
    );
    int wait_error = 0;
    io.schedule(
        [&]
        {
            sylar::HookScope hooks(1s);
            char byte;
            EXPECT_EQ(::read(fds[0], &byte, 1), -1);
            wait_error = errno;
        }
    );
    io.schedule(
        [&]
        {
            sylar::HookScope hooks(1s);
            EXPECT_EQ(::close(fds[0]), 0);
        }
    );
    io.run();
    ::close(fds[1]);
    EXPECT_EQ(wait_error, ECANCELED);
}

TEST(HookTest, VectorIoYieldsAndPreservesSegments)
{
    int fds[2];
    ASSERT_EQ(
        ::socketpair(
            AF_UNIX,
            SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC,
            0,
            fds
        ),
        0
    );
    sylar::IOManager io;
    char first[2]{};
    char second[2]{};
    io.schedule(
        [&]
        {
            sylar::HookScope hooks(1s);
            iovec buffers[] = {
                {first, sizeof(first)},
                {second, sizeof(second)}};
            EXPECT_EQ(::readv(fds[0], buffers, 2), 4);
        }
    );
    io.schedule(
        [&]
        {
            sylar::HookScope hooks(1s);
            char a[] = "ab";
            char b[] = "cd";
            iovec buffers[] = {{a, 2}, {b, 2}};
            EXPECT_EQ(::writev(fds[1], buffers, 2), 4);
        }
    );
    io.run();
    ::close(fds[0]);
    ::close(fds[1]);
    EXPECT_EQ(std::string(first, 2), "ab");
    EXPECT_EQ(std::string(second, 2), "cd");
}
