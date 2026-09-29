#pragma once

#include <chrono>
#include <cstddef>
#include <memory>
#include <string>

#include "db/Sqlite.hpp"

namespace sylar
{

class SqlitePoolState;
class SqlitePool;

class SqliteLease
{
public:
    ~SqliteLease() noexcept;
    SqliteLease(SqliteLease&& other) noexcept;
    SqliteLease& operator=(SqliteLease&& other) noexcept;
    SqliteLease(const SqliteLease&) = delete;
    SqliteLease& operator=(const SqliteLease&) = delete;

    SqliteDatabase& database() const;
    SqliteDatabase& operator*() const;
    SqliteDatabase* operator->() const;

private:
    friend class SqlitePool;
    SqliteLease(
        std::shared_ptr<SqlitePoolState> state,
        std::unique_ptr<SqliteDatabase> connection
    ) noexcept;
    void release() noexcept;

    std::shared_ptr<SqlitePoolState> m_state;
    std::unique_ptr<SqliteDatabase> m_connection;
};

class SqlitePool
{
public:
    SqlitePool(
        const std::string& filename,
        std::size_t connections,
        int busy_timeout_ms = 1000
    );
    SqlitePool(const SqlitePool&) = delete;
    SqlitePool& operator=(const SqlitePool&) = delete;

    // Blocks the calling OS thread. Use BlockingExecutor from an I/O
    // Fiber.
    SqliteLease acquire(
        std::chrono::milliseconds timeout = std::chrono::seconds(30)
    );
    std::size_t size() const noexcept;
    std::size_t available() const;

private:
    std::shared_ptr<SqlitePoolState> m_state;
    std::size_t m_size;
};

} // namespace sylar
