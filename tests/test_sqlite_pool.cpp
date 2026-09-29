#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <future>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>

#include <gtest/gtest.h>

#include "db/SqlitePool.hpp"
#include "fiber/BlockingExecutor.hpp"
#include "fiber/IOManager.hpp"

using namespace std::chrono_literals;

namespace
{

struct TemporaryDatabase
{
    TemporaryDatabase()
    {
        char pattern[] = "/tmp/sylar-sqlite-pool-XXXXXX";
        const int fd = ::mkstemp(pattern);
        if (fd < 0)
        {
            throw std::runtime_error("mkstemp failed");
        }
        ::close(fd);
        path = pattern;
    }

    ~TemporaryDatabase()
    {
        std::error_code error;
        std::filesystem::remove(path, error);
        std::filesystem::remove(path + "-wal", error);
        std::filesystem::remove(path + "-shm", error);
    }

    std::string path;
};

TEST(SqlitePoolTest, SeparatesConnectionsAndReturnsLeases)
{
    TemporaryDatabase file;
    sylar::SqlitePool pool(file.path, 2);
    EXPECT_EQ(pool.size(), 2);
    EXPECT_EQ(pool.available(), 2);
    {
        auto writer = pool.acquire();
        auto reader = pool.acquire();
        EXPECT_EQ(pool.available(), 0);
        writer->execute(
            "CREATE TABLE item(id INTEGER PRIMARY KEY, value TEXT)"
        );
        writer->execute("INSERT INTO item VALUES(1, 'visible')");
        auto query = reader->prepare(
            "SELECT value FROM item WHERE id = ?1"
        );
        query.bind(1, std::int64_t{1});
        ASSERT_TRUE(query.step());
        EXPECT_EQ(query.columnText(0), "visible");
    }
    EXPECT_EQ(pool.available(), 2);
    auto lease = pool.acquire(0ms);
    EXPECT_EQ(pool.available(), 1);
}

TEST(SqlitePoolTest, TimesOutWhenExhaustedAndRecovers)
{
    TemporaryDatabase file;
    sylar::SqlitePool pool(file.path, 1);
    std::optional<sylar::SqliteLease> lease{pool.acquire()};
    std::promise<bool> timed_out;
    auto result = timed_out.get_future();
    std::thread waiter(
        [&]
        {
            try
            {
                auto other = pool.acquire(30ms);
                timed_out.set_value(false);
            }
            catch (const std::system_error& error)
            {
                timed_out.set_value(
                    error.code().value() == ETIMEDOUT
                );
            }
        }
    );
    waiter.join();
    EXPECT_TRUE(result.get());
    lease.reset();
    EXPECT_EQ(pool.available(), 1);
    EXPECT_NO_THROW(pool.acquire(0ms));
}

TEST(SqlitePoolTest, LeaseKeepsConnectionAliveAfterPool)
{
    TemporaryDatabase file;
    std::optional<sylar::SqliteLease> lease;
    {
        sylar::SqlitePool pool(file.path, 1);
        lease.emplace(pool.acquire());
    }
    EXPECT_NO_THROW(
        lease->database().execute("CREATE TABLE alive(id INTEGER)")
    );
    lease.reset();
    EXPECT_THROW(
        sylar::SqlitePool(file.path, 0),
        std::invalid_argument
    );
    EXPECT_THROW(
        sylar::SqlitePool(":memory:", 1),
        std::invalid_argument
    );
}

TEST(SqlitePoolTest, RunsQueriesOffIoFiber)
{
    TemporaryDatabase file;
    sylar::SqlitePool pool(file.path, 2);
    {
        auto connection = pool.acquire();
        connection->execute("CREATE TABLE item(value INTEGER)");
        connection->execute("INSERT INTO item VALUES(7)");
    }
    sylar::BlockingExecutor blocking(2);
    sylar::IOManager io;
    std::atomic<int> total{0};
    for (int i = 0; i < 2; ++i)
    {
        io.schedule(
            [&]
            {
                const auto value = blocking.await(
                    io,
                    [&]
                    {
                        auto connection = pool.acquire();
                        auto query = connection->prepare(
                            "SELECT value FROM item"
                        );
                        if (!query.step())
                        {
                            throw std::runtime_error(
                                "missing SQLite row"
                            );
                        }
                        return static_cast<int>(query.columnInt64(0));
                    }
                );
                total.fetch_add(value);
            }
        );
    }
    io.run();
    EXPECT_EQ(total.load(), 14);
    EXPECT_EQ(pool.available(), 2);
}

} // namespace
