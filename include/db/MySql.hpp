#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace sylar
{

using MySqlRow = std::vector<std::optional<std::string>>;
using MySqlRows = std::vector<MySqlRow>;

class MySqlClient
{
public:
    static MySqlClient connectUnix(
        const std::string& socket_path,
        const std::string& user,
        const std::string& password,
        const std::string& database = {}
    );
    static MySqlClient connectTcp(
        const std::string& host,
        std::uint16_t port,
        const std::string& user,
        const std::string& password,
        const std::string& database = {},
        std::chrono::seconds timeout = std::chrono::seconds(5)
    );

    ~MySqlClient();
    MySqlClient(const MySqlClient&) = delete;
    MySqlClient& operator=(const MySqlClient&) = delete;
    MySqlClient(MySqlClient&&) noexcept;
    MySqlClient& operator=(MySqlClient&&) noexcept;

    std::uint64_t execute(std::string_view sql);
    MySqlRows query(std::string_view sql);
    std::uint64_t executePrepared(
        std::string_view sql,
        std::span<const std::string_view> arguments
    );
    MySqlRows queryPrepared(
        std::string_view sql,
        std::span<const std::string_view> arguments
    );

private:
    struct Impl;
    explicit MySqlClient(std::unique_ptr<Impl> impl) noexcept;

    std::unique_ptr<Impl> m_impl;
};

class MySqlTransaction
{
public:
    explicit MySqlTransaction(MySqlClient& client);
    ~MySqlTransaction() noexcept;
    MySqlTransaction(const MySqlTransaction&) = delete;
    MySqlTransaction& operator=(const MySqlTransaction&) = delete;

    void commit();
    void rollback();

private:
    MySqlClient& m_client;
    bool m_active = true;
};

} // namespace sylar