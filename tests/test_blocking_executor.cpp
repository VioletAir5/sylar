#include <atomic>
#include <chrono>
#include <cstdint>
#include <stdexcept>
#include <thread>

#include <gtest/gtest.h>

#include "db/Sqlite.hpp"
#include "fiber/BlockingExecutor.hpp"
#include "fiber/IOManager.hpp"

TEST(BlockingExecutorTest, WaitYieldsToOtherFibers)
{
    sylar::IOManager io;
    sylar::BlockingExecutor blocking(2);
    std::atomic<bool> other_ran = false;
    bool worker_saw_other = false;
    io.schedule(
        [&]
        {
            worker_saw_other = blocking.await(
                io,
                [&]
                {
                    std::this_thread::sleep_for(
                        std::chrono::milliseconds(20)
                    );
                    return other_ran.load();
                }
            );
        }
    );
    io.schedule([&] { other_ran.store(true); });
    io.run();
    EXPECT_TRUE(worker_saw_other);
}

TEST(BlockingExecutorTest, PropagatesValuesAndErrors)
{
    sylar::IOManager io;
    sylar::BlockingExecutor blocking(1);
    int value = 0;
    bool caught = false;
    io.schedule(
        [&]
        {
            value = blocking.await(io, [] { return 42; });
            blocking.await(io, [] {});
            try
            {
                blocking.await(
                    io,
                    []() -> int
                    { throw std::runtime_error("worker failure"); }
                );
            }
            catch (const std::runtime_error&)
            {
                caught = true;
            }
        }
    );
    io.run();
    EXPECT_EQ(value, 42);
    EXPECT_TRUE(caught);
}

TEST(BlockingExecutorTest, RunsSqliteOffIoThread)
{
    sylar::IOManager io;
    sylar::BlockingExecutor blocking(1);
    auto database = sylar::SqliteDatabase::memory();
    database.execute("CREATE TABLE item(value INTEGER)");
    std::thread::id io_thread;
    std::thread::id db_thread;
    std::int64_t result = 0;
    io.schedule(
        [&]
        {
            io_thread = std::this_thread::get_id();
            result = blocking.await(
                io,
                [&]() -> std::int64_t
                {
                    db_thread = std::this_thread::get_id();
                    database.execute("INSERT INTO item VALUES(7)");
                    auto query = database.prepare(
                        "SELECT value FROM item"
                    );
                    if (!query.step())
                    {
                        throw std::runtime_error("missing SQLite row"
                        );
                    }
                    return query.columnInt64(0);
                }
            );
        }
    );
    io.run();
    EXPECT_NE(io_thread, db_thread);
    EXPECT_EQ(result, 7);
}

TEST(BlockingExecutorTest, RejectsZeroThreads)
{
    EXPECT_THROW(sylar::BlockingExecutor(0), std::invalid_argument);
}
TEST(BlockingExecutorTest, StopCancelsWaitingFiber)
{
    sylar::IOManager io;
    sylar::BlockingExecutor blocking(1);
    bool cancelled = false;
    io.schedule(
        [&]
        {
            try
            {
                blocking.await(
                    io,
                    [] {
                        std::this_thread::sleep_for(
                            std::chrono::milliseconds(40)
                        );
                    }
                );
            }
            catch (const std::system_error& error)
            {
                cancelled = error.code().value() == ECANCELED;
            }
        }
    );
    io.addTimer(
        std::chrono::milliseconds(5),
        [&] { io.requestStop(); }
    );
    io.runForever();
    EXPECT_TRUE(cancelled);
}

TEST(BlockingExecutorTest, DeadlineSkipsQueuedOperation)
{
    sylar::IOManager io;
    sylar::BlockingExecutor blocking(1);
    std::atomic<bool> second_ran{false};
    bool timed_out = false;
    auto context = sylar::RequestContext::withTimeout(
        std::chrono::milliseconds(10)
    );
    io.schedule(
        [&]
        {
            blocking.await(
                io,
                [] {
                    std::this_thread::sleep_for(
                        std::chrono::milliseconds(60)
                    );
                }
            );
        }
    );
    io.schedule(
        [&]
        {
            try
            {
                blocking.await(
                    io,
                    context,
                    [&] { second_ran.store(true); }
                );
            }
            catch (const std::system_error& error)
            {
                timed_out = error.code().value() == ETIMEDOUT;
            }
        }
    );
    io.run();
    EXPECT_TRUE(timed_out);
    EXPECT_FALSE(second_ran.load());
}

TEST(BlockingExecutorTest, CancellationSkipsQueuedOperation)
{
    sylar::IOManager io;
    sylar::BlockingExecutor blocking(1);
    sylar::RequestContext context;
    std::atomic<bool> second_ran{false};
    bool cancelled = false;
    io.schedule(
        [&]
        {
            blocking.await(
                io,
                [] {
                    std::this_thread::sleep_for(
                        std::chrono::milliseconds(60)
                    );
                }
            );
        }
    );
    io.schedule(
        [&]
        {
            try
            {
                blocking.await(
                    io,
                    context,
                    [&] { second_ran.store(true); }
                );
            }
            catch (const std::system_error& error)
            {
                cancelled = error.code().value() == ECANCELED;
            }
        }
    );
    io.addTimer(
        std::chrono::milliseconds(10),
        [&] { context.cancel(); }
    );
    io.run();
    EXPECT_TRUE(cancelled);
    EXPECT_FALSE(second_ran.load());
}
