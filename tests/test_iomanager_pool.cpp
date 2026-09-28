#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <future>
#include <mutex>
#include <set>
#include <thread>

#include <gtest/gtest.h>

#include "fiber/IOManagerPool.hpp"

using namespace std::chrono_literals;

namespace
{

TEST(IOManagerPoolTest, PostsAcrossWorkersAndWakesIdleEpoll)
{
    sylar::IOManagerPool pool(3);
    std::atomic<int> completed{0};
    std::mutex mutex;
    std::set<std::thread::id> threads;
    std::promise<void> finished;
    auto future = finished.get_future();

    for (int index = 0; index < 30; ++index)
    {
        pool.post(
            [&](sylar::IOManager&)
            {
                {
                    std::lock_guard lock(mutex);
                    threads.insert(std::this_thread::get_id());
                }
                if (++completed == 30)
                {
                    finished.set_value();
                }
            }
        );
    }

    EXPECT_EQ(future.wait_for(2s), std::future_status::ready);
    pool.stop();
    EXPECT_EQ(threads.size(), 3);
    EXPECT_EQ(completed, 30);
    EXPECT_THROW(
        pool.post([](sylar::IOManager&) {}),
        std::logic_error
    );
}

TEST(IOManagerPoolTest, CompletesSocketWaitOnItsOwnerWorker)
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
    sylar::IOManagerPool pool(2);
    std::promise<sylar::IOManager::WaitResult> finished;
    auto future = finished.get_future();

    pool.post(
        [&](sylar::IOManager& io)
        {
            io.schedule(
                [&]
                { finished.set_value(io.waitReadable(fd[0], 1s)); }
            );
        }
    );
    const char byte = 'x';
    ASSERT_EQ(::write(fd[1], &byte, 1), 1);
    ASSERT_EQ(future.wait_for(2s), std::future_status::ready);
    EXPECT_EQ(future.get(), sylar::IOManager::WaitResult::Ready);
    pool.stop();
    ::close(fd[0]);
    ::close(fd[1]);
}

TEST(IOManagerPoolTest, StopCancelsPendingSocketWait)
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
    sylar::IOManagerPool pool(1);
    std::promise<void> registered;
    auto registration = registered.get_future();
    std::promise<sylar::IOManager::WaitResult> finished;
    auto future = finished.get_future();

    pool.post(
        [&](sylar::IOManager& io)
        {
            io.schedule(
                [&] { finished.set_value(io.waitReadable(fd[0])); }
            );
            io.addTimer(0ms, [&] { registered.set_value(); });
        }
    );
    ASSERT_EQ(registration.wait_for(2s), std::future_status::ready);
    pool.stop();

    ASSERT_EQ(future.wait_for(2s), std::future_status::ready);
    EXPECT_EQ(future.get(), sylar::IOManager::WaitResult::Cancelled);
    ::close(fd[0]);
    ::close(fd[1]);
}

TEST(IOManagerPoolTest, StopDrainsAlreadyPostedTasks)
{
    sylar::IOManagerPool pool(1);
    std::atomic<int> completed{0};
    for (int index = 0; index < 100; ++index)
    {
        pool.post([&](sylar::IOManager&) { ++completed; });
    }

    pool.stop();

    EXPECT_EQ(completed, 100);
}

} // namespace
