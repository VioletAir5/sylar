#include <charconv>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

#include "fiber/BlockingExecutor.hpp"
#include "fiber/IOManager.hpp"
#include "redis/RedisClient.hpp"

int main(int argc, char** argv)
{
    try
    {
        if (argc != 3)
        {
            throw std::invalid_argument("usage: redis_demo HOST PORT"
            );
        }
        unsigned port = 0;
        const std::string_view text(argv[2]);
        const auto [end, error] = std::from_chars(
            text.data(),
            text.data() + text.size(),
            port
        );
        if (error != std::errc{} ||
            end != text.data() + text.size() || port == 0 ||
            port > 65535)
        {
            throw std::invalid_argument("invalid Redis port");
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
                        auto redis = sylar::RedisClient::connect(
                            argv[1],
                            static_cast<std::uint16_t>(port)
                        );
                        if (redis.command({"PING"}).text != "PONG")
                        {
                            throw std::runtime_error(
                                "Redis PING failed"
                            );
                        }
                        const std::string binary("v\0x", 3);
                        redis.command(
                            {"SET",
                             "demo:key",
                             std::string_view(binary)}
                        );
                        if (redis.command({"GET", "demo:key"}).text !=
                            binary)
                        {
                            throw std::runtime_error(
                                "Redis binary GET failed"
                            );
                        }
                        if (redis.command({"INCR", "demo:counter"})
                                .integer != 1)
                        {
                            throw std::runtime_error(
                                "Redis INCR failed"
                            );
                        }
                        redis.command({"LPUSH", "demo:list", "a", "b"}
                        );
                        auto list = redis.command(
                            {"LRANGE", "demo:list", "0", "-1"}
                        );
                        if (list.type !=
                                sylar::RedisValue::Type::Array ||
                            list.elements.size() != 2 ||
                            list.elements[0].text != "b" ||
                            list.elements[1].text != "a")
                        {
                            throw std::runtime_error(
                                "Redis array failed"
                            );
                        }
                        if (redis.command({"GET", "missing"}).type !=
                            sylar::RedisValue::Type::Nil)
                        {
                            throw std::runtime_error(
                                "Redis NIL failed"
                            );
                        }
                        bool rejected = false;
                        try
                        {
                            redis.command({"NOT_A_COMMAND"});
                        }
                        catch (const std::runtime_error&)
                        {
                            rejected = true;
                        }
                        if (!rejected)
                        {
                            throw std::runtime_error(
                                "Redis error reply ignored"
                            );
                        }
                    }
                );
            }
        );
        io.run();
        std::cout << "redis ok" << std::endl;
    }
    catch (const std::exception& error)
    {
        std::cerr << "redis_demo: " << error.what() << std::endl;
        return 1;
    }
}