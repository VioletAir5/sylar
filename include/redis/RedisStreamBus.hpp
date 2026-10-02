#pragma once

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "redis/RedisClient.hpp"

namespace sylar
{

struct StreamEvent
{
    std::string id;
    std::string payload;
};

class ReplayGap final : public std::runtime_error
{
public:
    using std::runtime_error::runtime_error;
};

// Each replica uses XREAD with its own ID. Consumer groups are not
// used, since a group distributes rather than broadcasts events.
class RedisStreamBus
{
public:
    explicit RedisStreamBus(
        std::string key, std::size_t retention = 1000
    );
    std::string
    publish(RedisClient& redis, std::string_view payload) const;
    std::vector<StreamEvent> readAfter(
        RedisClient& redis,
        std::string_view last_id,
        std::uint32_t block_ms = 250
    ) const;
    static bool validId(std::string_view id) noexcept;

private:
    std::string m_key;
    std::string m_retention;
};

} // namespace sylar
