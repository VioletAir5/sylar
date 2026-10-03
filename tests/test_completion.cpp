#include <atomic>
#include <chrono>
#include <thread>

#include <gtest/gtest.h>

#include "fiber/IOManager.hpp"
using namespace std::chrono_literals;
TEST(CompletionTest, NotifyBeforeWaitAndDuplicateNotification)
{
    sylar::IOManager io;
    auto completion = io.makeCompletion();
    completion.notify();
    completion.notify();
    io.schedule(
        [&]
        {
            EXPECT_EQ(
                io.waitCompletion(
                    completion,
                    1s,
                    sylar::RequestContext{}
                ),
                sylar::IOManager::WaitResult::Ready
            );
        }
    );
    io.run();
    EXPECT_EQ(io.pendingEventCount(), 0);
}
TEST(CompletionTest, CrossThreadNotifyResumesOwner)
{
    sylar::IOManager io;
    auto completion = io.makeCompletion();
    auto owner = std::this_thread::get_id();
    std::jthread producer(
        [&]
        {
            std::this_thread::sleep_for(10ms);
            completion.notify();
        }
    );
    io.schedule(
        [&]
        {
            EXPECT_EQ(
                io.waitCompletion(
                    completion,
                    1s,
                    sylar::RequestContext{}
                ),
                sylar::IOManager::WaitResult::Ready
            );
            EXPECT_EQ(owner, std::this_thread::get_id());
        }
    );
    io.run();
}
TEST(CompletionTest, TimeoutCancellationAndLateNotification)
{
    sylar::IOManager io;
    auto first = io.makeCompletion(), second = io.makeCompletion();
    auto context = sylar::RequestContext::withTimeout(1s);
    io.schedule(
        [&]
        {
            EXPECT_EQ(
                io.waitCompletion(
                    first,
                    10ms,
                    sylar::RequestContext{}
                ),
                sylar::IOManager::WaitResult::TimedOut
            );
            first.notify();
        }
    );
    io.schedule(
        [&]
        {
            EXPECT_EQ(
                io.waitCompletion(second, 1s, context),
                sylar::IOManager::WaitResult::Cancelled
            );
        }
    );
    io.schedule(
        [&]
        {
            io.sleepFor(5ms);
            context.cancel();
        }
    );
    io.run();
    second.notify();
    io.run();
    EXPECT_EQ(io.pendingEventCount(), 0);
}
TEST(CompletionTest, OwnerStopAndDestructionRejectLatePosts)
{
    std::optional<sylar::IOManager::Completion> completion;
    {
        sylar::IOManager io;
        completion = io.makeCompletion();
        io.schedule(
            [&]
            {
                EXPECT_EQ(
                    io.waitCompletion(
                        *completion,
                        1s,
                        sylar::RequestContext{}
                    ),
                    sylar::IOManager::WaitResult::Cancelled
                );
            }
        );
        io.schedule(
            [&]
            {
                io.sleepFor(5ms);
                io.requestStop();
            }
        );
        io.runForever();
    }
    completion->notify();
    completion->notify();
}
