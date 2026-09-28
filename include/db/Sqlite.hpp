#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

struct sqlite3;
struct sqlite3_stmt;

namespace sylar
{

class SqliteStatement;

class SqliteDatabase
{
public:
    static SqliteDatabase open(const std::string& filename);
    static SqliteDatabase memory();

    SqliteDatabase(const SqliteDatabase&) = delete;
    SqliteDatabase& operator=(const SqliteDatabase&) = delete;
    SqliteDatabase(SqliteDatabase&&) noexcept = default;
    SqliteDatabase& operator=(SqliteDatabase&&) noexcept = default;

    void execute(std::string_view sql);
    SqliteStatement prepare(std::string_view sql);
    void busyTimeout(int milliseconds);

private:
    friend class SqliteTransaction;
    explicit SqliteDatabase(std::shared_ptr<sqlite3> connection
    ) noexcept;

    std::shared_ptr<sqlite3> m_connection;
};

class SqliteStatement
{
public:
    ~SqliteStatement();
    SqliteStatement(const SqliteStatement&) = delete;
    SqliteStatement& operator=(const SqliteStatement&) = delete;
    SqliteStatement(SqliteStatement&& other) noexcept;
    SqliteStatement& operator=(SqliteStatement&& other) noexcept;

    void bind(int index, std::int64_t value);
    void bind(int index, double value);
    void bind(int index, std::string_view value);
    void bindBlob(int index, std::span<const std::byte> value);
    void bindNull(int index);
    bool step();
    void reset();
    void clearBindings();

    int columnCount() const;
    bool columnIsNull(int index) const;
    std::int64_t columnInt64(int index) const;
    double columnDouble(int index) const;
    std::string columnText(int index) const;
    std::vector<std::byte> columnBlob(int index) const;

private:
    friend class SqliteDatabase;
    SqliteStatement(
        std::shared_ptr<sqlite3> connection, sqlite3_stmt* statement
    ) noexcept;

    void checkColumn(int index) const;
    void checkBind(int result) const;

    std::shared_ptr<sqlite3> m_connection;
    sqlite3_stmt* m_statement = nullptr;
    bool m_hasRow = false;
};

class SqliteTransaction
{
public:
    explicit SqliteTransaction(SqliteDatabase& database);
    ~SqliteTransaction();
    SqliteTransaction(const SqliteTransaction&) = delete;
    SqliteTransaction& operator=(const SqliteTransaction&) = delete;

    void commit();

private:
    std::shared_ptr<sqlite3> m_connection;
    bool m_active = true;
};

} // namespace sylar