#pragma once

#include <chrono>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

struct redisContext;

namespace sylar
{

struct RedisValue
{
    enum class Type
    {
        Status,
        String,
        Integer,
        Nil,
        Array
    };

    Type type = Type::Nil;
    std::string text;
    std::int64_t integer = 0;
    std::vector<RedisValue> elements;
};

class RedisClient
{
public:
    static RedisClient connect(
        const std::string& host,
        std::uint16_t port,
        std::chrono::milliseconds timeout = std::chrono::seconds(5)
    );

    RedisClient(const RedisClient&) = delete;
    RedisClient& operator=(const RedisClient&) = delete;
    RedisClient(RedisClient&&) noexcept = default;
    RedisClient& operator=(RedisClient&&) noexcept = default;

    RedisValue command(std::span<const std::string_view> arguments);
    RedisValue
    command(std::initializer_list<std::string_view> arguments);

private:
    struct ContextDeleter
    {
        void operator()(redisContext* context) const noexcept;
    };

    explicit RedisClient(redisContext* context) noexcept;
    std::unique_ptr<redisContext, ContextDeleter> m_context;
};

} // namespace sylar