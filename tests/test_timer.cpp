#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <stdexcept>
#include <system_error>

#include <gtest/gtest.h>

#include "fiber/IOManager.hpp"
#include "fiber/TimerManager.hpp"

using namespace std::chrono_literals;

namespace
{

TEST(TimerManagerTest, CancelsPendingTimer)
{
    sylar::TimerManager timers;
    bool fired = false;
    const auto id = timers.addAfter(0ms, [&] { fired = true; });

    EXPECT_TRUE(timers.cancel(id));
    EXPECT_FALSE(timers.cancel(id));
    EXPECT_TRUE(timers.takeExpired().empty());
    EXPECT_TRUE(timers.empty());
    EXPECT_FALSE(fired);
}

TEST(TimerManagerTest, RunsExpiredTimerOnce)
{
    sylar::TimerManager timers;
    const auto id = timers.addAfter(0ms, [] {});
    EXPECT_EQ(timers.nextTimeout(), 0ms);
    auto callbacks = timers.takeExpired();
    ASSERT_EQ(callbacks.size(), 1);
    callbacks.front()();
    EXPECT_FALSE(timers.cancel(id));
    EXPECT_TRUE(timers.empty());
}

TEST(IOManagerTimerTest, RunsTimerWithoutFileDescriptors)
{
    sylar::IOManager io;
    bool fired = false;
    io.addTimer(0ms, [&] { fired = true; });

    io.run();

    EXPECT_TRUE(fired);
}

TEST(IOManagerTimerTest, SleepResumesFiber)
{
    sylar::IOManager io;
    int stage = 0;
    io.schedule(
        [&]
        {
            stage = 1;
            io.sleepFor(1ms);
            stage = 2;
        }
    );

    io.run();

    EXPECT_EQ(stage, 2);
}

TEST(IOManagerTimerTest, ReadTimesOutAndRemovesRegistration)
{
    int fd[2];
    ASSERT_EQ(
        ::socketpair(
            AF_UNIX,
            SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC,
            0,
            fd
        ),
        0
    );
    sylar::IOManager io;
    auto result = sylar::IOManager::WaitResult::Ready;
    io.schedule([&] { result = io.waitReadable(fd[0], 1ms); });

    io.run();

    EXPECT_EQ(result, sylar::IOManager::WaitResult::TimedOut);
    EXPECT_EQ(io.pendingEventCount(), 0);
    ::close(fd[0]);
    ::close(fd[1]);
}

TEST(IOManagerTimerTest, ReadyEventCancelsTimeout)
{
    int fd[2];
    ASSERT_EQ(
        ::socketpair(
            AF_UNIX,
            SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC,
            0,
            fd
        ),
        0
    );
    sylar::IOManager io;
    auto result = sylar::IOManager::WaitResult::Cancelled;
    io.schedule([&] { result = io.waitReadable(fd[0], 100ms); });
    io.schedule(
        [&]
        {
            const char byte = 'x';
            ASSERT_EQ(::write(fd[1], &byte, 1), 1);
        }
    );

    io.run();

    EXPECT_EQ(result, sylar::IOManager::WaitResult::Ready);
    EXPECT_EQ(io.pendingEventCount(), 0);
    ::close(fd[0]);
    ::close(fd[1]);
}

} // namespace
