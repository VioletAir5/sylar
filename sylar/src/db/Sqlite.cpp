#include "db/Sqlite.hpp"

#include <algorithm>
#include <cctype>
#include <limits>
#include <stdexcept>
#include <utility>

#include <sqlite3.h>

namespace sylar
{
namespace
{

std::runtime_error
sqliteError(sqlite3* connection, const char* operation)
{
    return std::runtime_error(
        std::string(operation) + ": " + ::sqlite3_errmsg(connection)
    );
}

void executeSql(sqlite3* connection, std::string_view sql)
{
    if (sql.find(char{}) != std::string_view::npos)
    {
        throw std::invalid_argument("SQL contains NUL");
    }
    const std::string source(sql);
    char* error = nullptr;
    const auto result = ::sqlite3_exec(
        connection,
        source.c_str(),
        nullptr,
        nullptr,
        &error
    );
    if (result != SQLITE_OK)
    {
        const std::string detail = error
                                       ? error
                                       : ::sqlite3_errmsg(connection);
        ::sqlite3_free(error);
        throw std::runtime_error("sqlite3_exec: " + detail);
    }
}

} // namespace

SqliteDatabase::SqliteDatabase(std::shared_ptr<sqlite3> connection
) noexcept
    : m_connection(std::move(connection))
{
}

SqliteDatabase SqliteDatabase::open(const std::string& filename)
{
    sqlite3* raw = nullptr;
    const int result = ::sqlite3_open_v2(
        filename.c_str(),
        &raw,
        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE |
            SQLITE_OPEN_FULLMUTEX,
        nullptr
    );
    if (result != SQLITE_OK)
    {
        const std::string detail = raw ? ::sqlite3_errmsg(raw)
                                       : ::sqlite3_errstr(result);
        if (raw)
        {
            ::sqlite3_close_v2(raw);
        }
        throw std::runtime_error("sqlite3_open_v2: " + detail);
    }
    return SqliteDatabase(std::shared_ptr<sqlite3>(
        raw,
        [](sqlite3* connection) { ::sqlite3_close_v2(connection); }
    ));
}

SqliteDatabase SqliteDatabase::memory()
{
    return open(":memory:");
}

void SqliteDatabase::execute(std::string_view sql)
{
    executeSql(m_connection.get(), sql);
}

SqliteStatement SqliteDatabase::prepare(std::string_view sql)
{
    if (sql.find(char{}) != std::string_view::npos)
    {
        throw std::invalid_argument("SQL contains NUL");
    }
    if (sql.size() >
        static_cast<std::size_t>(std::numeric_limits<int>::max()))
    {
        throw std::length_error("SQL statement too large");
    }
    const std::string source(sql);
    sqlite3_stmt* statement = nullptr;
    const char* tail = nullptr;
    const int result = ::sqlite3_prepare_v3(
        m_connection.get(),
        source.c_str(),
        static_cast<int>(source.size()),
        0,
        &statement,
        &tail
    );
    if (result != SQLITE_OK)
    {
        throw sqliteError(m_connection.get(), "sqlite3_prepare_v3");
    }
    const bool extra = std::any_of(
        tail,
        source.c_str() + source.size(),
        [](unsigned char ch) { return !std::isspace(ch); }
    );
    if (!statement || extra)
    {
        ::sqlite3_finalize(statement);
        throw std::invalid_argument(
            "prepare requires exactly one SQL statement"
        );
    }
    return SqliteStatement(m_connection, statement);
}

void SqliteDatabase::busyTimeout(int milliseconds)
{
    if (milliseconds < 0)
    {
        throw std::invalid_argument("negative SQLite busy timeout");
    }
    if (::sqlite3_busy_timeout(m_connection.get(), milliseconds) !=
        SQLITE_OK)
    {
        throw sqliteError(m_connection.get(), "sqlite3_busy_timeout");
    }
}

SqliteStatement::SqliteStatement(
    std::shared_ptr<sqlite3> connection, sqlite3_stmt* statement
) noexcept
    : m_connection(std::move(connection))
    , m_statement(statement)
{
}

SqliteStatement::~SqliteStatement()
{
    ::sqlite3_finalize(m_statement);
}

SqliteStatement::SqliteStatement(SqliteStatement&& other) noexcept
    : m_connection(std::move(other.m_connection))
    , m_statement(std::exchange(other.m_statement, nullptr))
    , m_hasRow(std::exchange(other.m_hasRow, false))
{
}

SqliteStatement& SqliteStatement::operator=(SqliteStatement&& other
) noexcept
{
    if (this != &other)
    {
        ::sqlite3_finalize(m_statement);
        m_connection = std::move(other.m_connection);
        m_statement = std::exchange(other.m_statement, nullptr);
        m_hasRow = std::exchange(other.m_hasRow, false);
    }
    return *this;
}

void SqliteStatement::checkBind(int result) const
{
    if (result != SQLITE_OK)
    {
        throw sqliteError(m_connection.get(), "SQLite bind");
    }
}

void SqliteStatement::bind(int index, std::int64_t value)
{
    checkBind(::sqlite3_bind_int64(m_statement, index, value));
}

void SqliteStatement::bind(int index, double value)
{
    checkBind(::sqlite3_bind_double(m_statement, index, value));
}

void SqliteStatement::bind(int index, std::string_view value)
{
    checkBind(::sqlite3_bind_text64(
        m_statement,
        index,
        value.data(),
        value.size(),
        SQLITE_TRANSIENT,
        SQLITE_UTF8
    ));
}

void SqliteStatement::bindBlob(
    int index, std::span<const std::byte> value
)
{
    checkBind(::sqlite3_bind_blob64(
        m_statement,
        index,
        value.data(),
        value.size(),
        SQLITE_TRANSIENT
    ));
}

void SqliteStatement::bindNull(int index)
{
    checkBind(::sqlite3_bind_null(m_statement, index));
}

bool SqliteStatement::step()
{
    const auto result = ::sqlite3_step(m_statement);
    if (result == SQLITE_ROW)
    {
        m_hasRow = true;
        return true;
    }
    m_hasRow = false;
    if (result == SQLITE_DONE)
    {
        return false;
    }
    throw sqliteError(m_connection.get(), "sqlite3_step");
}

void SqliteStatement::reset()
{
    const auto result = ::sqlite3_reset(m_statement);
    m_hasRow = false;
    checkBind(result);
}

void SqliteStatement::clearBindings()
{
    checkBind(::sqlite3_clear_bindings(m_statement));
}

void SqliteStatement::checkColumn(int index) const
{
    if (!m_hasRow || index < 0 ||
        index >= ::sqlite3_column_count(m_statement))
    {
        throw std::out_of_range("SQLite column unavailable");
    }
}

int SqliteStatement::columnCount() const
{
    return ::sqlite3_column_count(m_statement);
}

bool SqliteStatement::columnIsNull(int index) const
{
    checkColumn(index);
    return ::sqlite3_column_type(m_statement, index) == SQLITE_NULL;
}

std::int64_t SqliteStatement::columnInt64(int index) const
{
    checkColumn(index);
    return ::sqlite3_column_int64(m_statement, index);
}

double SqliteStatement::columnDouble(int index) const
{
    checkColumn(index);
    return ::sqlite3_column_double(m_statement, index);
}

std::string SqliteStatement::columnText(int index) const
{
    checkColumn(index);
    const auto* data = ::sqlite3_column_text(m_statement, index);
    const auto count = ::sqlite3_column_bytes(m_statement, index);
    if (!data || count == 0)
    {
        return {};
    }
    return std::string(reinterpret_cast<const char*>(data), count);
}

std::vector<std::byte> SqliteStatement::columnBlob(int index) const
{
    checkColumn(index);
    const auto* data = ::sqlite3_column_blob(m_statement, index);
    const auto count = ::sqlite3_column_bytes(m_statement, index);
    if (!data || count == 0)
    {
        return {};
    }
    const auto* first = static_cast<const std::byte*>(data);
    return {first, first + count};
}

SqliteTransaction::SqliteTransaction(SqliteDatabase& database)
    : m_connection(database.m_connection)
{
    executeSql(m_connection.get(), "BEGIN IMMEDIATE");
}

SqliteTransaction::~SqliteTransaction()
{
    if (m_active)
    {
        try
        {
            executeSql(m_connection.get(), "ROLLBACK");
        }
        catch (...)
        {
        }
    }
}

void SqliteTransaction::commit()
{
    if (!m_active)
    {
        throw std::logic_error("SQLite transaction already completed"
        );
    }
    executeSql(m_connection.get(), "COMMIT");
    m_active = false;
}

} // namespace sylar