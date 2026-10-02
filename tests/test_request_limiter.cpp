#include <chrono>
#include <memory>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "http/RequestLimiter.hpp"

using namespace std::chrono_literals;

TEST(RequestLimiterTest, FixedLimitRejectsAndReleasesPermits)
{
    sylar::RequestLimiter limiter({
        .initial = 2,
        .minimum = 2,
        .maximum = 2,
        .window = 4,
    });
    auto first = limiter.tryAcquire();
    auto second = limiter.tryAcquire();
    ASSERT_TRUE(first);
    ASSERT_TRUE(second);
    EXPECT_FALSE(limiter.tryAcquire());
    EXPECT_EQ(limiter.snapshot().rejected, 1);
    EXPECT_EQ(limiter.snapshot().in_flight, 2);
    first.reset();
    EXPECT_TRUE(limiter.tryAcquire());
}

TEST(RequestLimiterTest, AdaptiveLimitBacksOffOnLatencyRise)
{
    sylar::RequestLimiter limiter({
        .initial = 4,
        .minimum = 2,
        .maximum = 4,
        .window = 4,
        .adaptive = true,
    });
    {
        std::vector<sylar::RequestLimiter::Permit> permits;
        for (int index = 0; index < 4; ++index)
        {
            auto permit = limiter.tryAcquire();
            ASSERT_TRUE(permit);
            permits.push_back(std::move(*permit));
        }
        std::this_thread::sleep_for(1ms);
    }
    EXPECT_EQ(limiter.snapshot().limit, 4);
    {
        std::vector<sylar::RequestLimiter::Permit> permits;
        for (int index = 0; index < 4; ++index)
        {
            auto permit = limiter.tryAcquire();
            ASSERT_TRUE(permit);
            permits.push_back(std::move(*permit));
        }
        std::this_thread::sleep_for(20ms);
    }
    EXPECT_EQ(limiter.snapshot().limit, 3);
    {
        std::vector<sylar::RequestLimiter::Permit> permits;
        for (int index = 0; index < 3; ++index)
        {
            auto permit = limiter.tryAcquire();
            ASSERT_TRUE(permit);
            permits.push_back(std::move(*permit));
        }
        std::this_thread::sleep_for(1ms);
    }
    {
        auto permit = limiter.tryAcquire();
        ASSERT_TRUE(permit);
        std::this_thread::sleep_for(1ms);
    }
    EXPECT_EQ(limiter.snapshot().limit, 4);
}
