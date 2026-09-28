#include <memory>
#include <stdexcept>
#include <vector>

#include <gtest/gtest.h>

#include "fiber/Scheduler.hpp"

namespace
{

TEST(SchedulerTest, RunsReadyFibersInRoundRobinOrder)
{
    sylar::Scheduler scheduler;
    std::vector<int> events;

    auto first = scheduler.schedule(
        [&]
        {
            EXPECT_EQ(sylar::Scheduler::GetThis(), &scheduler);
            events.push_back(1);
            sylar::Fiber::YieldToReady();
            events.push_back(3);
        }
    );
    auto second = scheduler.schedule(
        [&]
        {
            events.push_back(2);
            sylar::Fiber::YieldToReady();
            events.push_back(4);
        }
    );

    scheduler.run();

    EXPECT_EQ(sylar::Scheduler::GetThis(), nullptr);
    EXPECT_EQ(events, (std::vector<int>{1, 2, 3, 4}));
    EXPECT_EQ(first->getState(), sylar::Fiber::TERM);
    EXPECT_EQ(second->getState(), sylar::Fiber::TERM);
    EXPECT_EQ(scheduler.readyCount(), 0);
    EXPECT_EQ(scheduler.heldCount(), 0);
}

TEST(SchedulerTest, HeldFiberWaitsUntilWoken)
{
    sylar::Scheduler scheduler;
    std::vector<int> events;
    auto fiber = scheduler.schedule(
        [&]
        {
            events.push_back(1);
            sylar::Fiber::YieldToHold();
            events.push_back(2);
        }
    );

    scheduler.run();

    EXPECT_EQ(events, (std::vector<int>{1}));
    EXPECT_EQ(fiber->getState(), sylar::Fiber::HOLD);
    EXPECT_EQ(scheduler.heldCount(), 1);
    EXPECT_FALSE(scheduler.wake(0));
    EXPECT_TRUE(scheduler.wake(fiber->getId()));

    scheduler.run();

    EXPECT_EQ(events, (std::vector<int>{1, 2}));
    EXPECT_EQ(scheduler.heldCount(), 0);
    EXPECT_FALSE(scheduler.wake(fiber->getId()));
}

TEST(SchedulerTest, AnotherFiberCanWakeHeldFiber)
{
    sylar::Scheduler scheduler;
    std::vector<int> events;
    auto held = scheduler.schedule(
        [&]
        {
            events.push_back(1);
            sylar::Fiber::YieldToHold();
            events.push_back(3);
        }
    );
    scheduler.schedule(
        [&]
        {
            events.push_back(2);
            EXPECT_TRUE(scheduler.wake(held->getId()));
        }
    );

    scheduler.run();

    EXPECT_EQ(events, (std::vector<int>{1, 2, 3}));
    EXPECT_EQ(scheduler.heldCount(), 0);
}

TEST(SchedulerTest, CancellingHeldFiberUnwindsStack)
{
    struct Guard
    {
        int& destroyed;
        ~Guard()
        {
            ++destroyed;
        }
    };

    sylar::Scheduler scheduler;
    int destroyed = 0;
    auto fiber = scheduler.schedule(
        [&]
        {
            Guard guard{destroyed};
            sylar::Fiber::YieldToHold();
        }
    );
    const auto id = fiber->getId();
    fiber.reset();

    scheduler.run();
    EXPECT_EQ(destroyed, 0);
    EXPECT_TRUE(scheduler.cancel(id));
    EXPECT_EQ(destroyed, 1);
    EXPECT_EQ(scheduler.heldCount(), 0);
}

TEST(SchedulerTest, ReportsFailureAfterOtherReadyTasksRun)
{
    sylar::Scheduler scheduler;
    bool second_ran = false;
    scheduler.schedule([]
                       { throw std::runtime_error("fiber failed"); });
    scheduler.schedule([&] { second_ran = true; });

    EXPECT_THROW(scheduler.run(), std::runtime_error);
    EXPECT_TRUE(second_ran);
    EXPECT_EQ(scheduler.readyCount(), 0);
}

TEST(SchedulerTest, RejectsDuplicateSchedulingAndManagedReset)
{
    sylar::Scheduler scheduler;
    auto fiber = scheduler.schedule([] {});
    EXPECT_THROW(scheduler.schedule(fiber), std::invalid_argument);
    EXPECT_THROW(fiber->swapIn(), std::logic_error);
    EXPECT_THROW(fiber->reset([] {}), std::logic_error);

    scheduler.run();

    EXPECT_NO_THROW(fiber->reset([] {}));
}

} // namespace
