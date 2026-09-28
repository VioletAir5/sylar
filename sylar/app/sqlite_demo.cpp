#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string_view>

#include "db/Sqlite.hpp"

int main(int argc, char** argv)
{
    try
    {
        if (argc != 1 && argc != 2)
        {
            throw std::invalid_argument(
                "usage: sqlite_demo [database-file]"
            );
        }
        auto database = argc == 2
                            ? sylar::SqliteDatabase::open(argv[1])
                            : sylar::SqliteDatabase::memory();
        database.execute(
            "CREATE TABLE IF NOT EXISTS metrics("
            "name TEXT PRIMARY KEY, value INTEGER NOT NULL)"
        );
        {
            sylar::SqliteTransaction transaction(database);
            auto statement = database.prepare(
                "INSERT INTO metrics(name, value) VALUES(?1, ?2) "
                "ON CONFLICT(name) DO UPDATE SET "
                "value = value + excluded.value"
            );
            statement.bind(1, "requests");
            statement.bind(2, std::int64_t{1});
            statement.step();
            transaction.commit();
        }
        auto query = database.prepare(
            "SELECT value FROM metrics WHERE name = ?1"
        );
        query.bind(1, "requests");
        if (query.step())
        {
            std::cout << "requests=" << query.columnInt64(0)
                      << std::endl;
        }
    }
    catch (const std::exception& error)
    {
        std::cerr << "sqlite_demo: " << error.what() << std::endl;
        return 1;
    }
}