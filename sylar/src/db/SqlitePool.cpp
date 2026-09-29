#include "db/SqlitePool.hpp"

#include <cerrno>
#include <condition_variable>
#include <mutex>
#include <stdexcept>
#include <system_error>
#include <utility>
#include <vector>

namespace sylar
{

class SqlitePoolState
{
public:
    mutable std::mutex mutex;
    std::condition_variable ready;
    std::vector<std::unique_ptr<SqliteDatabase>> idle;
};

SqliteLease::SqliteLease(
    std::shared_ptr<SqlitePoolState> state,
    std::unique_ptr<SqliteDatabase> connection
) noexcept
    : m_state(std::move(state))
    , m_connection(std::move(connection))
{
}

SqliteLease::~SqliteLease() noexcept
{
    release();
}

SqliteLease::SqliteLease(SqliteLease&& other) noexcept
    : m_state(std::move(other.m_state))
    , m_connection(std::move(other.m_connection))
{
}

SqliteLease& SqliteLease::operator=(SqliteLease&& other) noexcept
{
    if (this != &other)
    {
        release();
        m_state = std::move(other.m_state);
        m_connection = std::move(other.m_connection);
    }
    return *this;
}

void SqliteLease::release() noexcept
{
    if (!m_connection)
    {
        return;
    }
    {
        std::lock_guard lock(m_state->mutex);
        m_state->idle.push_back(std::move(m_connection));
    }
    m_state->ready.notify_one();
    m_state.reset();
}

SqliteDatabase& SqliteLease::database() const
{
    if (!m_connection)
    {
        throw std::logic_error("empty SQLite lease");
    }
    return *m_connection;
}

SqliteDatabase& SqliteLease::operator*() const
{
    return database();
}

SqliteDatabase* SqliteLease::operator->() const
{
    return &database();
}

SqlitePool::SqlitePool(
    const std::string& filename,
    std::size_t connections,
    int busy_timeout_ms
)
    : m_state(std::make_shared<SqlitePoolState>())
    , m_size(connections)
{
    if (filename.empty() || filename == ":memory:" ||
        filename.find(char{}) != std::string::npos ||
        connections == 0 || busy_timeout_ms < 0)
    {
        throw std::invalid_argument("invalid SQLite pool options");
    }
    m_state->idle.reserve(connections);
    for (std::size_t i = 0; i < connections; ++i)
    {
        auto database = std::make_unique<SqliteDatabase>(
            SqliteDatabase::open(filename)
        );
        database->busyTimeout(busy_timeout_ms);
        m_state->idle.push_back(std::move(database));
    }
}

SqliteLease SqlitePool::acquire(std::chrono::milliseconds timeout)
{
    if (timeout.count() < 0)
    {
        throw std::invalid_argument("negative SQLite pool timeout");
    }
    std::unique_lock lock(m_state->mutex);
    if (!m_state->ready.wait_for(
            lock,
            timeout,
            [this] { return !m_state->idle.empty(); }
        ))
    {
        throw std::system_error(
            ETIMEDOUT,
            std::generic_category(),
            "SQLite pool acquire"
        );
    }
    auto connection = std::move(m_state->idle.back());
    m_state->idle.pop_back();
    return SqliteLease(m_state, std::move(connection));
}

std::size_t SqlitePool::size() const noexcept
{
    return m_size;
}

std::size_t SqlitePool::available() const
{
    std::lock_guard lock(m_state->mutex);
    return m_state->idle.size();
}

} // namespace sylar
