#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>

#include <gtest/gtest.h>

#include "db/Sqlite.hpp"

TEST(SqliteTest, BindsValuesAndReadsTypedRows)
{
    auto database = sylar::SqliteDatabase::memory();
    database.execute("CREATE TABLE item("
                     "id INTEGER, name TEXT, payload BLOB, "
                     "score REAL, nullable TEXT)");
    auto insert = database.prepare(
        "INSERT INTO item VALUES(?1, ?2, ?3, ?4, ?5)"
    );
    insert.bind(1, std::int64_t{42});
    insert.bind(2, "hello");
    const std::array<std::byte, 4> bytes{
        std::byte{0},
        std::byte{1},
        std::byte{255},
        std::byte{0}};
    insert.bindBlob(3, bytes);
    insert.bind(4, 3.5);
    insert.bindNull(5);
    EXPECT_FALSE(insert.step());

    auto query = database.prepare(
        "SELECT id, name, payload, score, nullable "
        "FROM item WHERE id = ?1"
    );
    query.bind(1, std::int64_t{42});
    ASSERT_TRUE(query.step());
    EXPECT_EQ(query.columnCount(), 5);
    EXPECT_EQ(query.columnInt64(0), 42);
    EXPECT_EQ(query.columnText(1), "hello");
    EXPECT_EQ(
        query.columnBlob(2),
        (std::vector<std::byte>(bytes.begin(), bytes.end()))
    );
    EXPECT_DOUBLE_EQ(query.columnDouble(3), 3.5);
    EXPECT_TRUE(query.columnIsNull(4));
    EXPECT_FALSE(query.step());
    EXPECT_THROW(query.columnText(1), std::out_of_range);

    query.reset();
    query.clearBindings();
    query.bind(1, std::int64_t{42});
    EXPECT_TRUE(query.step());
}

TEST(SqliteTest, CommitsAndRollsBackTransactions)
{
    auto database = sylar::SqliteDatabase::memory();
    database.execute("CREATE TABLE item(value INTEGER)");
    {
        sylar::SqliteTransaction transaction(database);
        database.execute("INSERT INTO item VALUES(1)");
    }
    {
        auto count = database.prepare("SELECT COUNT(*) FROM item");
        ASSERT_TRUE(count.step());
        EXPECT_EQ(count.columnInt64(0), 0);
    }
    {
        sylar::SqliteTransaction transaction(database);
        database.execute("INSERT INTO item VALUES(2)");
        transaction.commit();
        EXPECT_THROW(transaction.commit(), std::logic_error);
    }
    auto count = database.prepare("SELECT COUNT(*) FROM item");
    ASSERT_TRUE(count.step());
    EXPECT_EQ(count.columnInt64(0), 1);
}

TEST(SqliteTest, RejectsInvalidStatementsAndBounds)
{
    auto database = sylar::SqliteDatabase::memory();
    EXPECT_THROW(
        database.prepare("SELECT 1; SELECT 2"),
        std::invalid_argument
    );
    EXPECT_THROW(database.prepare("SELECT FROM"), std::runtime_error);
    EXPECT_THROW(database.prepare(""), std::invalid_argument);
    EXPECT_THROW(database.busyTimeout(-1), std::invalid_argument);
    auto statement = database.prepare("SELECT ?1");
    EXPECT_THROW(
        statement.bind(2, std::int64_t{1}),
        std::runtime_error
    );
    EXPECT_THROW(statement.columnInt64(0), std::out_of_range);
}

TEST(SqliteTest, StatementKeepsConnectionAlive)
{
    std::optional<sylar::SqliteStatement> statement;
    {
        auto database = sylar::SqliteDatabase::memory();
        statement.emplace(database.prepare("SELECT 7"));
    }
    ASSERT_TRUE(statement->step());
    EXPECT_EQ(statement->columnInt64(0), 7);
}
TEST(SqliteTest, TransactionKeepsConnectionAlive)
{
    std::optional<sylar::SqliteTransaction> transaction;
    {
        auto database = sylar::SqliteDatabase::memory();
        database.execute("CREATE TABLE item(value INTEGER)");
        transaction.emplace(database);
        database.execute("INSERT INTO item VALUES(9)");
    }
    EXPECT_NO_THROW(transaction->commit());
}
