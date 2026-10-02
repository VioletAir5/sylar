#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <stdexcept>
#include <string>
#include <system_error>

#include <gtest/gtest.h>

#include "fiber/IOManager.hpp"

namespace
{

class SocketPair
{
public:
    explicit SocketPair(bool nonblocking = true)
    {
        const int type = SOCK_STREAM | SOCK_CLOEXEC |
                         (nonblocking ? SOCK_NONBLOCK : 0);
        if (::socketpair(AF_UNIX, type, 0, m_fd) != 0)
        {
            throw std::system_error(
                errno,
                std::generic_category(),
                "socketpair"
            );
        }
    }

    ~SocketPair()
    {
        closeFirst();
        closeSecond();
    }

    int first() const noexcept
    {
        return m_fd[0];
    }

    int second() const noexcept
    {
        return m_fd[1];
    }

    void closeFirst() noexcept
    {
        if (m_fd[0] >= 0)
        {
            ::close(m_fd[0]);
            m_fd[0] = -1;
        }
    }

    void closeSecond() noexcept
    {
        if (m_fd[1] >= 0)
        {
            ::close(m_fd[1]);
            m_fd[1] = -1;
        }
    }

private:
    int m_fd[2]{-1, -1};
};

TEST(IOManagerTest, ResumesReaderAfterSocketBecomesReadable)
{
    SocketPair sockets;
    sylar::IOManager io;
    std::string received;
    sylar::IOManager::WaitResult
        result = sylar::IOManager::WaitResult::Cancelled;

    io.schedule(
        [&]
        {
            result = io.waitReadable(sockets.first());
            char buffer[8]{};
            const auto count = ::read(
                sockets.first(),
                buffer,
                sizeof(buffer)
            );
            if (count > 0)
            {
                received.assign(
                    buffer,
                    static_cast<std::size_t>(count)
                );
            }
        }
    );
    io.schedule(
        [&]
        {
            const char message[] = "hello";
            EXPECT_EQ(::write(sockets.second(), message, 5), 5);
        }
    );

    io.run();

    EXPECT_EQ(result, sylar::IOManager::WaitResult::Ready);
    EXPECT_EQ(received, "hello");
    EXPECT_EQ(io.pendingEventCount(), 0);
}

TEST(IOManagerTest, SupportsReadAndWriteWaitersOnOneFd)
{
    SocketPair sockets;
    sylar::IOManager io;
    bool read_ready = false;
    bool write_ready = false;

    io.schedule(
        [&]
        {
            read_ready = io.waitReadable(sockets.first()) ==
                         sylar::IOManager::WaitResult::Ready;
        }
    );
    io.schedule(
        [&]
        {
            write_ready = io.waitWritable(sockets.first()) ==
                          sylar::IOManager::WaitResult::Ready;
        }
    );
    io.schedule(
        [&]
        {
            const char message = 'x';
            EXPECT_EQ(::write(sockets.second(), &message, 1), 1);
        }
    );

    io.run();

    EXPECT_TRUE(read_ready);
    EXPECT_TRUE(write_ready);
    EXPECT_EQ(io.pendingEventCount(), 0);
}

TEST(IOManagerTest, CancelWakesWaiterWithCancelledResult)
{
    SocketPair sockets;
    sylar::IOManager io;
    sylar::IOManager::WaitResult
        result = sylar::IOManager::WaitResult::Ready;
    bool cancelled = false;

    io.schedule([&] { result = io.waitReadable(sockets.first()); });
    io.schedule(
        [&]
        {
            cancelled = io.cancel(
                sockets.first(),
                sylar::IOManager::Event::Read
            );
        }
    );

    io.run();

    EXPECT_TRUE(cancelled);
    EXPECT_EQ(result, sylar::IOManager::WaitResult::Cancelled);
    EXPECT_EQ(io.pendingEventCount(), 0);
}

TEST(IOManagerTest, PeerCloseWakesReader)
{
    SocketPair sockets;
    sylar::IOManager io;
    ssize_t read_count = -1;

    io.schedule(
        [&]
        {
            io.waitReadable(sockets.first());
            char byte;
            read_count = ::read(sockets.first(), &byte, 1);
        }
    );
    io.schedule([&] { sockets.closeSecond(); });

    io.run();

    EXPECT_EQ(read_count, 0);
}

TEST(IOManagerTest, CanCancelAfterWatchedFdWasClosed)
{
    SocketPair sockets;
    sylar::IOManager io;
    sylar::IOManager::WaitResult
        result = sylar::IOManager::WaitResult::Ready;
    bool cancelled = false;
    const int watched_fd = sockets.first();

    io.schedule([&] { result = io.waitReadable(watched_fd); });
    io.schedule(
        [&]
        {
            sockets.closeFirst();
            cancelled = io.cancel(
                watched_fd,
                sylar::IOManager::Event::Read
            );
        }
    );

    io.run();

    EXPECT_TRUE(cancelled);
    EXPECT_EQ(result, sylar::IOManager::WaitResult::Cancelled);
    EXPECT_EQ(io.pendingEventCount(), 0);
}

TEST(IOManagerTest, RejectsBlockingSocket)
{
    SocketPair sockets(false);
    sylar::IOManager io;
    io.schedule([&] { io.waitReadable(sockets.first()); });

    EXPECT_THROW(io.run(), std::invalid_argument);
    EXPECT_EQ(io.pendingEventCount(), 0);
}

} // namespace

TEST(IOManagerTest, RequestContextDeadlineAndCancellation)
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
    auto deadline = sylar::RequestContext::withTimeout(
        std::chrono::milliseconds(10)
    );
    sylar::RequestContext cancelled;
    sylar::IOManager::WaitResult timeout_result{};
    sylar::IOManager::WaitResult cancel_result{};
    io.schedule(
        [&]
        {
            timeout_result = io.waitReadable(fds[0], deadline);
            cancel_result = io.waitReadable(fds[0], cancelled);
        }
    );
    io.addTimer(
        std::chrono::milliseconds(20),
        [&] { cancelled.cancel(); }
    );
    io.run();
    EXPECT_EQ(timeout_result, sylar::IOManager::WaitResult::TimedOut);
    EXPECT_EQ(cancel_result, sylar::IOManager::WaitResult::Cancelled);
    EXPECT_EQ(io.pendingEventCount(), 0);
    ::close(fds[0]);
    ::close(fds[1]);
}
