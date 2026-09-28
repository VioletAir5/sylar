#include "redis/RedisClient.hpp"

#include <climits>
#include <stdexcept>
#include <string_view>
#include <utility>

#include <hiredis/hiredis.h>

namespace sylar
{
namespace
{

std::string replyText(const redisReply* reply)
{
    if (!reply->str || reply->len == 0)
    {
        return {};
    }
    if (reply->len > 16 * 1024 * 1024)
    {
        throw std::length_error("Redis reply string too large");
    }
    return std::string(reply->str, reply->len);
}

RedisValue
convert(const redisReply* reply, int depth, std::size_t& remaining)
{
    if (!reply || depth > 32 || remaining-- == 0)
    {
        throw std::runtime_error("invalid or oversized Redis reply");
    }
    RedisValue value;
    switch (reply->type)
    {
    case REDIS_REPLY_STATUS:
        value.type = RedisValue::Type::Status;
        value.text = replyText(reply);
        break;
    case REDIS_REPLY_STRING:
        value.type = RedisValue::Type::String;
        value.text = replyText(reply);
        break;
    case REDIS_REPLY_INTEGER:
        value.type = RedisValue::Type::Integer;
        value.integer = reply->integer;
        break;
    case REDIS_REPLY_NIL:
        value.type = RedisValue::Type::Nil;
        break;
    case REDIS_REPLY_ARRAY:
        if (reply->elements > remaining)
        {
            throw std::length_error("Redis reply array too large");
        }
        value.type = RedisValue::Type::Array;
        value.elements.reserve(reply->elements);
        for (std::size_t i = 0; i < reply->elements; ++i)
        {
            value.elements.push_back(
                convert(reply->element[i], depth + 1, remaining)
            );
        }
        break;
    case REDIS_REPLY_ERROR:
        throw std::runtime_error("Redis error: " + replyText(reply));
    default:
        throw std::runtime_error("unknown Redis reply type");
    }
    return value;
}

} // namespace

void RedisClient::ContextDeleter::operator()(redisContext* context
) const noexcept
{
    if (context)
    {
        ::redisFree(context);
    }
}

RedisClient::RedisClient(redisContext* context) noexcept
    : m_context(context)
{
}

RedisClient RedisClient::connect(
    const std::string& host,
    std::uint16_t port,
    std::chrono::milliseconds timeout
)
{
    if (host.empty() || host.find(char{}) != std::string::npos ||
        port == 0 || timeout.count() <= 0)
    {
        throw std::invalid_argument("invalid Redis connection options"
        );
    }
    const timeval duration{
        static_cast<time_t>(timeout.count() / 1000),
        static_cast<suseconds_t>(timeout.count() % 1000 * 1000)};
    redisContext* context = ::redisConnectWithTimeout(
        host.c_str(),
        port,
        duration
    );
    if (!context)
    {
        throw std::runtime_error("Redis connection allocation failed"
        );
    }
    if (context->err)
    {
        const std::string detail(context->errstr);
        ::redisFree(context);
        throw std::runtime_error("Redis connect: " + detail);
    }
    if (::redisSetTimeout(context, duration) != REDIS_OK)
    {
        const std::string detail(context->errstr);
        ::redisFree(context);
        throw std::runtime_error("Redis timeout: " + detail);
    }
    return RedisClient(context);
}

RedisValue
RedisClient::command(std::span<const std::string_view> arguments)
{
    if (arguments.empty() || arguments.size() > INT_MAX)
    {
        throw std::invalid_argument("invalid Redis command");
    }
    std::vector<const char*> values;
    std::vector<std::size_t> lengths;
    values.reserve(arguments.size());
    lengths.reserve(arguments.size());
    for (const auto argument : arguments)
    {
        values.push_back(argument.data());
        lengths.push_back(argument.size());
    }
    std::unique_ptr<redisReply, decltype(&::freeReplyObject)> reply(
        static_cast<redisReply*>(::redisCommandArgv(
            m_context.get(),
            static_cast<int>(arguments.size()),
            values.data(),
            lengths.data()
        )),
        &::freeReplyObject
    );
    if (!reply)
    {
        throw std::runtime_error(
            std::string("Redis command: ") + m_context->errstr
        );
    }
    std::size_t remaining = 100000;
    return convert(reply.get(), 0, remaining);
}

RedisValue
RedisClient::command(std::initializer_list<std::string_view> arguments
)
{
    return command(std::span<const std::string_view>(
        arguments.begin(),
        arguments.size()
    ));
}

} // namespace sylar