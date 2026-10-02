#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <span>
#include <stdexcept>
#include <string>
#include <system_error>

#include <gtest/gtest.h>

#include "net/SocketStream.hpp"

#include "fiber/IOManager.hpp"
#include "fiber/RequestContext.hpp"

TEST(StreamTest, TransfersWholeBufferAcrossFibers)
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
    sylar::SocketStream reader(io, sylar::Socket::adopt(fds[0]));
    sylar::SocketStream writer(io, sylar::Socket::adopt(fds[1]));
    std::string output;
    io.schedule(
        [&]
        {
            std::array<std::byte, 5> buffer{};
            reader.readExact(buffer);
            output.assign(
                reinterpret_cast<const char*>(buffer.data()),
                buffer.size()
            );
        }
    );
    io.schedule(
        [&]
        {
            const std::string input = "hello";
            writer.writeAll(std::as_bytes(std::span(input)));
        }
    );
    io.run();
    EXPECT_EQ(output, "hello");
}

TEST(StreamTest, RequestContextLimitsSocketRead)
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
    sylar::SocketStream reader(
        io,
        sylar::Socket::adopt(fds[0]),
        std::chrono::seconds(1)
    );
    auto context = std::make_shared<sylar::RequestContext>(
        std::chrono::steady_clock::now() +
        std::chrono::milliseconds(10)
    );
    reader.setRequestContext(context);
    bool timed_out = false;
    io.schedule(
        [&]
        {
            std::array<std::byte, 1> buffer{};
            try
            {
                (void)reader.readSome(buffer);
            }
            catch (const std::system_error& error)
            {
                timed_out = error.code().value() == ETIMEDOUT;
            }
        }
    );
    io.run();
    EXPECT_TRUE(timed_out);
    ::close(fds[1]);
}
