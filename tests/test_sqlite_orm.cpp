#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "db/SqliteOrm.hpp"

namespace
{
struct User
{
    std::int64_t id = 0;
    std::string name;
    std::optional<std::int64_t> age;
    std::vector<std::byte> avatar;
    double score = 0;
};

struct IdOnly
{
    std::int64_t id = 0;
};

TEST(SqliteOrm, CreateSaveUpdateFindAndErase)
{
    auto database = sylar::SqliteDatabase::memory();
    sylar::SqliteRepository<
        User,
        std::string,
        std::optional<std::int64_t>,
        std::vector<std::byte>,
        double>
        repository(
            database,
            "users",
            &User::id,
            sylar::sqliteColumn("name", &User::name),
            sylar::sqliteColumn("age", &User::age),
            sylar::sqliteColumn("avatar", &User::avatar),
            sylar::sqliteColumn("score", &User::score)
        );
    repository.createTable();
    repository.createTable();
    EXPECT_FALSE(repository.find(7).has_value());

    User user{
        7,
        "Alice",
        std::nullopt,
        {std::byte{0}, std::byte{255}},
        1.25};
    repository.save(user);
    auto found = repository.find(7);
    ASSERT_TRUE(found);
    EXPECT_EQ(found->id, 7);
    EXPECT_EQ(found->name, "Alice");
    EXPECT_FALSE(found->age);
    EXPECT_EQ(found->avatar, user.avatar);
    EXPECT_DOUBLE_EQ(found->score, 1.25);

    user.name = "Bob";
    user.age = 42;
    user.avatar.clear();
    repository.save(user);
    found = repository.find(7);
    ASSERT_TRUE(found);
    EXPECT_EQ(found->name, "Bob");
    EXPECT_EQ(found->age, 42);
    EXPECT_TRUE(found->avatar.empty());
    repository.erase(7);
    EXPECT_FALSE(repository.find(7));
}

TEST(SqliteOrm, IdOnlyAndIdentifierValidation)
{
    auto database = sylar::SqliteDatabase::memory();
    sylar::SqliteRepository<IdOnly> repository(
        database,
        "ids",
        &IdOnly::id
    );
    repository.createTable();
    repository.save({19});
    ASSERT_TRUE(repository.find(19));
    repository.erase(19);
    EXPECT_FALSE(repository.find(19));

    EXPECT_THROW(
        (sylar::SqliteRepository<IdOnly>(
            database,
            "ids;DROP",
            &IdOnly::id
        )),
        std::invalid_argument
    );
    EXPECT_THROW(
        (sylar::SqliteRepository<IdOnly>(
            database,
            "9ids",
            &IdOnly::id
        )),
        std::invalid_argument
    );
}
TEST(SqliteOrm, ReservedIdentifierAndDuplicateMapping)
{
    auto database = sylar::SqliteDatabase::memory();
    sylar::SqliteRepository<User, std::string> repository(
        database,
        "group",
        &User::id,
        sylar::sqliteColumn("order", &User::name)
    );
    repository.createTable();
    repository.save(User{3, "quoted"});
    ASSERT_TRUE(repository.find(3));
    EXPECT_EQ(repository.find(3)->name, "quoted");

    EXPECT_THROW(
        (sylar::SqliteRepository<User, std::string>(
            database,
            "users",
            &User::id,
            sylar::sqliteColumn("ID", &User::name)
        )),
        std::invalid_argument
    );
    EXPECT_THROW(
        (sylar::SqliteRepository<User, std::string, std::string>(
            database,
            "users",
            &User::id,
            sylar::sqliteColumn("Name", &User::name),
            sylar::sqliteColumn("name", &User::name)
        )),
        std::invalid_argument
    );
}

TEST(SqliteOrm, IntegerRangeIsChecked)
{
    struct Counter
    {
        std::int64_t id = 0;
        std::uint64_t count = 0;
    };
    auto database = sylar::SqliteDatabase::memory();
    sylar::SqliteRepository<Counter, std::uint64_t> repository(
        database,
        "counters",
        &Counter::id,
        sylar::sqliteColumn("count", &Counter::count)
    );
    repository.createTable();
    repository.save({1, 42});
    ASSERT_TRUE(repository.find(1));
    EXPECT_EQ(repository.find(1)->count, 42);
    EXPECT_THROW(repository.save({2, UINT64_MAX}), std::out_of_range);
    database.execute("INSERT INTO counters(id, count) VALUES(3, -1)");
    EXPECT_THROW(repository.find(3), std::out_of_range);
}

} // namespace