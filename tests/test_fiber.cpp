#include <memory>
#include <stdexcept>
#include <vector>

#include <gtest/gtest.h>

#include "config/Config.hpp"
#include "fiber/Fiber.hpp"

namespace
{

void nestedYield(std::vector<int>& events)
{
    int local = 7;
    events.push_back(local);
    sylar::Fiber::YieldToReady();
    events.push_back(local + 1);
}

TEST(FiberTest, ResumesNestedCallStack)
{
    auto main = sylar::Fiber::GetThis();
    ASSERT_EQ(main->getId(), 0);
    ASSERT_EQ(main->getState(), sylar::Fiber::EXEC);

    std::vector<int> events;
    auto fiber = std::make_shared<sylar::Fiber>(
        [&events]
        {
            nestedYield(events);
            events.push_back(9);
        }
    );

    fiber->swapIn();
    EXPECT_EQ(events, (std::vector<int>{7}));
    EXPECT_EQ(fiber->getState(), sylar::Fiber::READY);
    EXPECT_EQ(sylar::Fiber::GetThis(), main);

    fiber->swapIn();
    EXPECT_EQ(events, (std::vector<int>{7, 8, 9}));
    EXPECT_EQ(fiber->getState(), sylar::Fiber::TERM);
    EXPECT_THROW(fiber->swapIn(), std::logic_error);
}

TEST(FiberTest, HoldsAndResetsAfterCompletion)
{
    int count = 0;
    auto fiber = std::make_shared<sylar::Fiber>(
        [&count]
        {
            ++count;
            sylar::Fiber::YieldToHold();
            ++count;
        }
    );

    fiber->swapIn();
    EXPECT_EQ(count, 1);
    EXPECT_EQ(fiber->getState(), sylar::Fiber::HOLD);

    fiber->swapIn();
    EXPECT_EQ(count, 2);
    EXPECT_EQ(fiber->getState(), sylar::Fiber::TERM);

    fiber->reset([&count] { count += 10; });
    EXPECT_EQ(fiber->getState(), sylar::Fiber::INIT);
    fiber->swapIn();
    EXPECT_EQ(count, 12);
    EXPECT_EQ(fiber->getState(), sylar::Fiber::TERM);
}

TEST(FiberTest, ReadsDefaultStackSizeFromConfig)
{
    auto configured = sylar::Config::Lookup<std::uint32_t>(
        "fiber.stack_size",
        128 * 1024
    );
    ASSERT_NE(configured, nullptr);

    const auto original = configured->getValue();
    configured->setValue(64 * 1024);
    auto fiber = std::make_shared<sylar::Fiber>([] {});
    EXPECT_EQ(fiber->getStackSize(), 64 * 1024);
    configured->setValue(original);
}

TEST(FiberTest, CallerModeUsesCallAndBack)
{
    std::vector<int> events;
    auto fiber = std::make_shared<sylar::Fiber>(
        [&events]
        {
            events.push_back(1);
            sylar::Fiber::YieldToReady();
            events.push_back(2);
            sylar::Fiber::GetCurrent()->back();
            events.push_back(3);
        },
        0,
        true
    );

    EXPECT_THROW(fiber->swapIn(), std::logic_error);
    fiber->call();
    EXPECT_EQ(fiber->getState(), sylar::Fiber::READY);
    fiber->call();
    EXPECT_EQ(fiber->getState(), sylar::Fiber::HOLD);
    fiber->call();
    EXPECT_EQ(fiber->getState(), sylar::Fiber::TERM);
    EXPECT_EQ(events, (std::vector<int>{1, 2, 3}));
}

TEST(FiberTest, CapturesExceptionWithoutCrossingContext)
{
    auto fiber = std::make_shared<sylar::Fiber>(
        [] { throw std::runtime_error("fiber failure"); }
    );

    fiber->swapIn();

    EXPECT_EQ(fiber->getState(), sylar::Fiber::EXCEPT);
    EXPECT_THROW(fiber->rethrowIfFailed(), std::runtime_error);
}

TEST(FiberTest, DestroyingSuspendedFiberUnwindsItsStack)
{
    struct Guard
    {
        int& destructed;
        ~Guard()
        {
            ++destructed;
        }
    };

    int destructed = 0;
    {
        auto fiber = std::make_shared<sylar::Fiber>(
            [&destructed]
            {
                Guard guard{destructed};
                sylar::Fiber::YieldToHold();
            }
        );

        fiber->swapIn();
        EXPECT_EQ(destructed, 0);
    }

    EXPECT_EQ(destructed, 1);
}

} // namespace
