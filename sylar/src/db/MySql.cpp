#include "db/MySql.hpp"

#include <climits>
#include <limits>
#include <stdexcept>
#include <utility>

#include <mysql/mysql.h>

namespace sylar
{
namespace
{

struct ConnectionDeleter
{
    void operator()(MYSQL* connection) const noexcept
    {
        if (connection)
        {
            ::mysql_close(connection);
        }
    }
};

struct StatementDeleter
{
    void operator()(MYSQL_STMT* statement) const noexcept
    {
        if (statement)
        {
            ::mysql_stmt_close(statement);
        }
    }
};

struct ResultDeleter
{
    void operator()(MYSQL_RES* result) const noexcept
    {
        if (result)
        {
            ::mysql_free_result(result);
        }
    }
};

using Statement = std::unique_ptr<MYSQL_STMT, StatementDeleter>;
using Result = std::unique_ptr<MYSQL_RES, ResultDeleter>;

void validateSql(std::string_view sql)
{
    if (sql.empty() || sql.find(char{}) != std::string_view::npos ||
        sql.size() > ULONG_MAX)
    {
        throw std::invalid_argument("invalid MySQL statement");
    }
}

std::runtime_error error(MYSQL* connection, const char* step)
{
    return std::runtime_error(
        std::string(step) + ": " + ::mysql_error(connection)
    );
}

std::runtime_error
statementError(MYSQL_STMT* statement, const char* step)
{
    return std::runtime_error(
        std::string(step) + ": " + ::mysql_stmt_error(statement)
    );
}

std::unique_ptr<MYSQL, ConnectionDeleter> openConnection(
    const char* host,
    unsigned int port,
    const char* socket,
    const std::string& user,
    const std::string& password,
    const std::string& database,
    unsigned int timeout
)
{
    if (user.find(char{}) != std::string::npos ||
        password.find(char{}) != std::string::npos ||
        database.find(char{}) != std::string::npos)
    {
        throw std::invalid_argument("MySQL option contains NUL");
    }
    std::unique_ptr<MYSQL, ConnectionDeleter> connection(
        ::mysql_init(nullptr)
    );
    if (!connection)
    {
        throw std::runtime_error("mysql_init failed");
    }
    if (::mysql_options(
            connection.get(),
            MYSQL_OPT_CONNECT_TIMEOUT,
            &timeout
        ) != 0 ||
        ::mysql_options(
            connection.get(),
            MYSQL_OPT_READ_TIMEOUT,
            &timeout
        ) != 0 ||
        ::mysql_options(
            connection.get(),
            MYSQL_OPT_WRITE_TIMEOUT,
            &timeout
        ) != 0)
    {
        throw error(connection.get(), "mysql_options");
    }
    if (!::mysql_real_connect(
            connection.get(),
            host,
            user.c_str(),
            password.c_str(),
            database.empty() ? nullptr : database.c_str(),
            port,
            socket,
            0
        ))
    {
        throw error(connection.get(), "mysql_real_connect");
    }
    if (::mysql_set_character_set(connection.get(), "utf8mb4") != 0)
    {
        throw error(connection.get(), "mysql_set_character_set");
    }
    return connection;
}

MySqlRows copyRows(MYSQL* connection, MYSQL_RES* result)
{
    MySqlRows rows;
    const auto count = ::mysql_num_fields(result);
    while (auto* values = ::mysql_fetch_row(result))
    {
        if (rows.size() >= 100000)
        {
            throw std::length_error("MySQL result too large");
        }
        const auto* lengths = ::mysql_fetch_lengths(result);
        if (!lengths)
        {
            throw error(connection, "mysql_fetch_lengths");
        }
        MySqlRow row;
        row.reserve(count);
        for (unsigned int i = 0; i < count; ++i)
        {
            if (!values[i])
            {
                row.emplace_back(std::nullopt);
            }
            else
            {
                if (lengths[i] > 16 * 1024 * 1024)
                {
                    throw std::length_error("MySQL cell too large");
                }
                row.emplace_back(std::string(values[i], lengths[i]));
            }
        }
        rows.push_back(std::move(row));
    }
    if (::mysql_errno(connection) != 0)
    {
        throw error(connection, "mysql_fetch_row");
    }
    return rows;
}

Statement runPrepared(
    MYSQL* connection,
    std::string_view sql,
    std::span<const std::string_view> arguments
)
{
    validateSql(sql);
    Statement statement(::mysql_stmt_init(connection));
    if (!statement)
    {
        throw error(connection, "mysql_stmt_init");
    }
    if (::mysql_stmt_prepare(
            statement.get(),
            sql.data(),
            static_cast<unsigned long>(sql.size())
        ) != 0)
    {
        throw statementError(statement.get(), "mysql_stmt_prepare");
    }
    if (::mysql_stmt_param_count(statement.get()) != arguments.size())
    {
        throw std::invalid_argument("MySQL parameter count mismatch");
    }
    std::vector<MYSQL_BIND> binds(arguments.size());
    std::vector<unsigned long> lengths(arguments.size());
    static char empty = 0;
    for (std::size_t i = 0; i < arguments.size(); ++i)
    {
        if (arguments[i].size() > ULONG_MAX)
        {
            throw std::length_error("MySQL parameter too large");
        }
        lengths[i] = static_cast<unsigned long>(arguments[i].size());
        binds[i].buffer_type = MYSQL_TYPE_STRING;
        binds[i].buffer = const_cast<char*>(
            arguments[i].empty() ? &empty : arguments[i].data()
        );
        binds[i].buffer_length = lengths[i];
        binds[i].length = &lengths[i];
    }
    if (!binds.empty() &&
        ::mysql_stmt_bind_param(statement.get(), binds.data()) != 0)
    {
        throw statementError(
            statement.get(),
            "mysql_stmt_bind_param"
        );
    }
    if (::mysql_stmt_execute(statement.get()) != 0)
    {
        throw statementError(statement.get(), "mysql_stmt_execute");
    }
    return statement;
}

MySqlRows copyPreparedRows(MYSQL_STMT* statement)
{
    Result metadata(::mysql_stmt_result_metadata(statement));
    if (!metadata)
    {
        if (::mysql_stmt_field_count(statement) != 0)
        {
            throw statementError(
                statement,
                "mysql_stmt_result_metadata"
            );
        }
        return {};
    }
    if (::mysql_stmt_store_result(statement) != 0)
    {
        throw statementError(statement, "mysql_stmt_store_result");
    }
    const auto count = ::mysql_num_fields(metadata.get());
    std::vector<MYSQL_BIND> binds(count);
    std::vector<unsigned long> lengths(count);
    auto nulls = std::make_unique<bool[]>(count);
    auto errors = std::make_unique<bool[]>(count);
    std::vector<char> empty(count);
    for (unsigned int i = 0; i < count; ++i)
    {
        binds[i].buffer_type = MYSQL_TYPE_STRING;
        binds[i].buffer = &empty[i];
        binds[i].buffer_length = 0;
        binds[i].length = &lengths[i];
        binds[i].is_null = &nulls[i];
        binds[i].error = &errors[i];
    }
    if (count != 0 &&
        ::mysql_stmt_bind_result(statement, binds.data()) != 0)
    {
        throw statementError(statement, "mysql_stmt_bind_result");
    }
    MySqlRows rows;
    for (;;)
    {
        const auto fetched = ::mysql_stmt_fetch(statement);
        if (fetched == MYSQL_NO_DATA)
        {
            break;
        }
        if (fetched != 0 && fetched != MYSQL_DATA_TRUNCATED)
        {
            throw statementError(statement, "mysql_stmt_fetch");
        }
        if (rows.size() >= 100000)
        {
            throw std::length_error("MySQL result too large");
        }
        MySqlRow row;
        row.reserve(count);
        for (unsigned int i = 0; i < count; ++i)
        {
            if (nulls[i])
            {
                row.emplace_back(std::nullopt);
                continue;
            }
            if (lengths[i] > 16 * 1024 * 1024)
            {
                throw std::length_error("MySQL cell too large");
            }
            std::string value(lengths[i], char{});
            MYSQL_BIND column{};
            column.buffer_type = MYSQL_TYPE_STRING;
            column.buffer = value.empty() ? &empty[i] : value.data();
            column.buffer_length = lengths[i];
            if (::mysql_stmt_fetch_column(statement, &column, i, 0) !=
                0)
            {
                throw statementError(
                    statement,
                    "mysql_stmt_fetch_column"
                );
            }
            row.emplace_back(std::move(value));
        }
        rows.push_back(std::move(row));
    }
    return rows;
}

} // namespace

struct MySqlClient::Impl
{
    explicit Impl(std::unique_ptr<MYSQL, ConnectionDeleter> value)
        : connection(std::move(value))
    {
    }

    std::unique_ptr<MYSQL, ConnectionDeleter> connection;
};

MySqlClient::MySqlClient(std::unique_ptr<Impl> impl) noexcept
    : m_impl(std::move(impl))
{
}

MySqlClient::~MySqlClient() = default;
MySqlClient::MySqlClient(MySqlClient&&) noexcept = default;
MySqlClient& MySqlClient::operator=(MySqlClient&&) noexcept = default;

MySqlClient MySqlClient::connectUnix(
    const std::string& socket_path,
    const std::string& user,
    const std::string& password,
    const std::string& database
)
{
    if (socket_path.empty() ||
        socket_path.find(char{}) != std::string::npos)
    {
        throw std::invalid_argument("invalid MySQL socket path");
    }
    return MySqlClient(std::make_unique<Impl>(openConnection(
        "localhost",
        0,
        socket_path.c_str(),
        user,
        password,
        database,
        5
    )));
}

MySqlClient MySqlClient::connectTcp(
    const std::string& host,
    std::uint16_t port,
    const std::string& user,
    const std::string& password,
    const std::string& database,
    std::chrono::seconds timeout
)
{
    if (host.empty() || host.find(char{}) != std::string::npos ||
        port == 0 || timeout.count() <= 0 ||
        timeout.count() > UINT_MAX)
    {
        throw std::invalid_argument("invalid MySQL TCP options");
    }
    return MySqlClient(std::make_unique<Impl>(openConnection(
        host.c_str(),
        port,
        nullptr,
        user,
        password,
        database,
        static_cast<unsigned int>(timeout.count())
    )));
}

std::uint64_t MySqlClient::execute(std::string_view sql)
{
    validateSql(sql);
    auto* connection = m_impl->connection.get();
    if (::mysql_real_query(
            connection,
            sql.data(),
            static_cast<unsigned long>(sql.size())
        ) != 0)
    {
        throw error(connection, "mysql_real_query");
    }
    Result result(::mysql_store_result(connection));
    if (!result && ::mysql_field_count(connection) != 0)
    {
        throw error(connection, "mysql_store_result");
    }
    return ::mysql_affected_rows(connection);
}

MySqlRows MySqlClient::query(std::string_view sql)
{
    validateSql(sql);
    auto* connection = m_impl->connection.get();
    if (::mysql_real_query(
            connection,
            sql.data(),
            static_cast<unsigned long>(sql.size())
        ) != 0)
    {
        throw error(connection, "mysql_real_query");
    }
    Result result(::mysql_store_result(connection));
    if (!result)
    {
        if (::mysql_field_count(connection) != 0)
        {
            throw error(connection, "mysql_store_result");
        }
        return {};
    }
    return copyRows(connection, result.get());
}

std::uint64_t MySqlClient::executePrepared(
    std::string_view sql, std::span<const std::string_view> arguments
)
{
    auto statement = runPrepared(
        m_impl->connection.get(),
        sql,
        arguments
    );
    return ::mysql_stmt_affected_rows(statement.get());
}

MySqlRows MySqlClient::queryPrepared(
    std::string_view sql, std::span<const std::string_view> arguments
)
{
    auto statement = runPrepared(
        m_impl->connection.get(),
        sql,
        arguments
    );
    return copyPreparedRows(statement.get());
}

} // namespace sylar