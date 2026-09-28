#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <cstddef>
#include <span>
#include <stdexcept>
#include <string>

#include <gtest/gtest.h>

#include "net/SocketStream.hpp"

#include "fiber/IOManager.hpp"

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
