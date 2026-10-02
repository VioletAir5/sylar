#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstddef>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <system_error>

#include <gtest/gtest.h>

#include "net/Socket.hpp"
#include "net/SocketStream.hpp"

#include "fiber/IOManager.hpp"
#include "http/OutboundBudget.hpp"
#include "http/WebSocket.hpp"

namespace
{

class MemoryStream final : public sylar::Stream
{
public:
    std::size_t readSome(std::span<std::byte>) override
    {
        return 0;
    }

    std::size_t writeSome(std::span<const std::byte> bytes) override
    {
        if (fail)
        {
            throw std::runtime_error("injected write failure");
        }
        written.append(
            reinterpret_cast<const char*>(bytes.data()),
            bytes.size()
        );
        return bytes.size();
    }

    bool fail = false;
    std::string written;
};

sylar::HttpRequest upgrade()
{
    sylar::HttpRequest request;
    request.method = "GET";
    request.target = "/ws";
    request.version = "HTTP/1.1";
    request.headers["host"] = "localhost";
    request.headers["upgrade"] = "websocket";
    request.headers["connection"] = "Upgrade";
    request.headers["sec-websocket-version"] = "13";
    request.headers["sec-websocket-key"] = "dGhlIHNhbXBsZSBub25jZQ==";
    return request;
}

} // namespace

TEST(OutboundBudgetTest, BoundsConcurrentBytesAndReleasesOnMove)
{
    auto budget = std::make_shared<sylar::OutboundBudget>(32);
    auto first = budget->tryReserve(20);
    ASSERT_TRUE(first);
    EXPECT_EQ(budget->inFlightBytes(), 20);
    EXPECT_FALSE(budget->tryReserve(13));
    EXPECT_EQ(budget->rejected(), 1);
    auto moved = std::move(first);
    first.reset();
    EXPECT_EQ(budget->inFlightBytes(), 20);
    moved.reset();
    EXPECT_EQ(budget->inFlightBytes(), 0);
    EXPECT_TRUE(budget->tryReserve(32));
    EXPECT_EQ(budget->inFlightBytes(), 0);
}

TEST(
    OutboundBudgetTest,
    WebSocketRejectsFullBudgetAndReleasesOnWriteFailure
)
{
    auto budget = std::make_shared<sylar::OutboundBudget>(24);
    MemoryStream stream;
    sylar::WebSocketSession
        session(stream, upgrade(), {}, 1024, budget);
    auto held = budget->tryReserve(20);
    ASSERT_TRUE(held);
    EXPECT_THROW(session.sendText("hello"), std::system_error);
    EXPECT_EQ(budget->inFlightBytes(), 20);
    held.reset();
    stream.fail = true;
    EXPECT_THROW(session.sendText("hello"), std::runtime_error);
    EXPECT_EQ(budget->inFlightBytes(), 0);
    stream.fail = false;
    EXPECT_NO_THROW(session.sendText("hello"));
    EXPECT_EQ(budget->inFlightBytes(), 0);
}

TEST(OutboundBudgetTest, SlowSocketTimesOutWithoutStoppingOtherFiber)
{
    int fds[2];
    ASSERT_EQ(
        ::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, fds),
        0
    );
    const int small = 4096;
    ASSERT_EQ(
        ::setsockopt(
            fds[0],
            SOL_SOCKET,
            SO_SNDBUF,
            &small,
            sizeof(small)
        ),
        0
    );
    sylar::IOManager io;
    auto budget = std::make_shared<sylar::OutboundBudget>(
        1024 * 1024
    );
    bool other_ran = false;
    bool timed_out = false;
    io.schedule(
        [&]
        {
            sylar::SocketStream stream(
                io,
                sylar::Socket::borrow(fds[0])
            );
            sylar::WebSocketSession session(
                stream,
                upgrade(),
                {},
                1024 * 1024,
                budget,
                std::chrono::milliseconds(50)
            );
            try
            {
                session.sendBinary(std::string(512 * 1024, 'x'));
            }
            catch (const std::system_error& error)
            {
                timed_out = error.code().value() == ETIMEDOUT;
            }
        }
    );
    io.schedule([&] { other_ran = true; });
    io.run();
    EXPECT_TRUE(other_ran);
    EXPECT_TRUE(timed_out);
    EXPECT_EQ(budget->inFlightBytes(), 0);
    ::close(fds[0]);
    ::close(fds[1]);
}
