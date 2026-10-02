#include "redis/RedisStreamBus.hpp"

#include <charconv>
#include <limits>
#include <stdexcept>
#include <string_view>

namespace sylar
{
namespace
{

using Type = RedisValue::Type;

std::pair<std::uint64_t, std::uint64_t> parseId(std::string_view text)
{
    const auto dash = text.find('-');
    if (dash == std::string_view::npos || dash == 0 ||
        dash + 1 == text.size())
    {
        throw std::invalid_argument("invalid Redis Stream ID");
    }
    std::uint64_t major = 0;
    std::uint64_t minor = 0;
    const auto [mend, me] = std::from_chars(
        text.data(),
        text.data() + dash,
        major
    );
    const auto [send, se] = std::from_chars(
        text.data() + dash + 1,
        text.data() + text.size(),
        minor
    );
    if (me != std::errc{} || se != std::errc{} ||
        mend != text.data() + dash ||
        send != text.data() + text.size())
    {
        throw std::invalid_argument("invalid Redis Stream ID");
    }
    return {major, minor};
}

std::string_view scalar(const RedisValue& value)
{
    if (value.type != Type::String && value.type != Type::Status)
    {
        throw std::runtime_error("invalid Redis Stream reply");
    }
    return value.text;
}

StreamEvent eventFrom(const RedisValue& value)
{
    if (value.type != Type::Array || value.elements.size() != 2)
    {
        throw std::runtime_error("invalid Redis Stream entry");
    }
    StreamEvent event;
    event.id = scalar(value.elements[0]);
    const auto& fields = value.elements[1];
    if (fields.type != Type::Array || fields.elements.size() != 2 ||
        scalar(fields.elements[0]) != "data")
    {
        throw std::runtime_error("invalid Redis Stream fields");
    }
    event.payload = scalar(fields.elements[1]);
    return event;
}

} // namespace

RedisStreamBus::RedisStreamBus(std::string key, std::size_t retention)
    : m_key(std::move(key))
    , m_retention(std::to_string(retention))
{
    if (m_key.empty() || m_key.size() > 128 || retention == 0 ||
        retention > 1000000)
    {
        throw std::invalid_argument("invalid Redis Stream options");
    }
}

bool RedisStreamBus::validId(std::string_view id) noexcept
{
    try
    {
        (void)parseId(id);
        return true;
    }
    catch (...)
    {
        return false;
    }
}

std::string RedisStreamBus::publish(
    RedisClient& redis, std::string_view payload
) const
{
    if (payload.size() > 1024 * 1024)
    {
        throw std::length_error("Stream payload too large");
    }
    auto reply = redis.command(
        {"XADD",
         m_key,
         "MAXLEN",
         "=",
         m_retention,
         "*",
         "data",
         payload}
    );
    return std::string(scalar(reply));
}

std::vector<StreamEvent> RedisStreamBus::readAfter(
    RedisClient& redis,
    std::string_view last_id,
    std::uint32_t block_ms
) const
{
    if (!validId(last_id) || block_ms == 0 || block_ms > 1000)
    {
        throw std::invalid_argument("invalid Stream read options");
    }
    auto oldest = redis.command(
        {"XRANGE", m_key, "-", "+", "COUNT", "1"}
    );
    if (oldest.type != Type::Array)
    {
        throw std::runtime_error("invalid Redis Stream oldest reply");
    }
    if (!oldest.elements.empty() && last_id != "0-0" &&
        parseId(last_id) < parseId(eventFrom(oldest.elements[0]).id))
    {
        throw ReplayGap("Redis Stream replay window expired");
    }
    const auto timeout = std::to_string(block_ms);
    auto reply = redis.command(
        {"XREAD",
         "BLOCK",
         timeout,
         "COUNT",
         "32",
         "STREAMS",
         m_key,
         last_id}
    );
    if (reply.type == Type::Nil)
    {
        return {};
    }
    if (reply.type != Type::Array || reply.elements.size() != 1)
    {
        throw std::runtime_error("invalid Redis XREAD reply");
    }
    const auto& stream = reply.elements[0];
    if (stream.type != Type::Array || stream.elements.size() != 2 ||
        scalar(stream.elements[0]) != m_key ||
        stream.elements[1].type != Type::Array)
    {
        throw std::runtime_error("invalid Redis XREAD stream");
    }
    std::vector<StreamEvent> events;
    for (const auto& item : stream.elements[1].elements)
    {
        events.push_back(eventFrom(item));
    }
    return events;
}

} // namespace sylar
