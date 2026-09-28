#include <array>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

#include "db/MySql.hpp"
#include "fiber/BlockingExecutor.hpp"
#include "fiber/IOManager.hpp"

int main(int argc, char** argv)
{
    try
    {
        if (argc != 2)
        {
            throw std::invalid_argument(
                "usage: mysql_demo SOCKET_PATH"
            );
        }
        sylar::IOManager io;
        sylar::BlockingExecutor blocking(1);
        io.schedule(
            [&]
            {
                blocking.await(
                    io,
                    [&]
                    {
                        auto mysql = sylar::MySqlClient::connectUnix(
                            argv[1],
                            "root",
                            ""
                        );
                        mysql.execute("CREATE DATABASE "
                                      "IF NOT EXISTS "
                                      "sylar_demo");
                        mysql.execute("USE sylar_demo");
                        mysql.execute("CREATE TABLE "
                                      "IF NOT EXISTS item("
                                      "id INT PRIMARY KEY, "
                                      "payload BLOB, "
                                      "note TEXT NULL)");
                        const std::string binary("v\0x", 3);
                        const std::array<std::string_view, 2> insert{
                            "1",
                            std::string_view(binary)};
                        mysql.executePrepared(
                            "INSERT INTO item("
                            "id, payload, note)"
                            " VALUES(?, ?, NULL)"
                            " ON DUPLICATE KEY "
                            "UPDATE payload = "
                            "VALUES(payload)",
                            insert
                        );
                        const std::array<std::string_view, 1> key{
                            "1"};
                        const auto rows = mysql.queryPrepared(
                            "SELECT payload, "
                            "note FROM item "
                            "WHERE id = ?",
                            key
                        );
                        if (rows.size() != 1 || rows[0].size() != 2 ||
                            !rows[0][0] || *rows[0][0] != binary ||
                            rows[0][1])
                        {
                            throw std::runtime_error("prepared MySQL "
                                                     "result mismatch"
                            );
                        }
                        const auto count = mysql.query(
                            "SELECT COUNT(*) "
                            "FROM item"
                        );
                        if (count.size() != 1 || !count[0][0] ||
                            *count[0][0] != "1")
                        {
                            throw std::runtime_error("MySQL count "
                                                     "mismatch");
                        }
                        bool rejected = false;
                        try
                        {
                            mysql.queryPrepared("SELECT ?", {});
                        }
                        catch (const std::invalid_argument&)
                        {
                            rejected = true;
                        }
                        if (!rejected)
                        {
                            throw std::runtime_error(
                                "MySQL parameter "
                                "count ignored"
                            );
                        }
                    }
                );
            }
        );
        io.run();
        std::cout << "mysql ok" << std::endl;
    }
    catch (const std::exception& error)
    {
        std::cerr << "mysql_demo: " << error.what() << std::endl;
        return 1;
    }
}