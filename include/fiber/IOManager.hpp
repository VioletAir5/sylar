#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <thread>
#include <unordered_map>

#include "fiber/Scheduler.hpp"
#include "fiber/TimerManager.hpp"

namespace sylar
{

// Linux epoll integration for one owner thread and one Scheduler.
class IOManager
{
public:
    enum class Event
    {
        Read,
        Write
    };

    enum class WaitResult
    {
        Ready,
        Cancelled,
        TimedOut
    };

    IOManager();
    ~IOManager();

    IOManager(const IOManager&) = delete;
    IOManager& operator=(const IOManager&) = delete;
    IOManager(IOManager&&) = delete;
    IOManager& operator=(IOManager&&) = delete;

    Fiber::ptr schedule(
        std::function<void()> callback, std::size_t stacksize = 0
    );

    // Call these inside a Fiber scheduled by this IOManager.
    // The file descriptor must be nonblocking.
    WaitResult waitReadable(int fd);
    WaitResult waitWritable(int fd);
    WaitResult
    waitReadable(int fd, std::chrono::milliseconds timeout);
    WaitResult
    waitWritable(int fd, std::chrono::milliseconds timeout);

    TimerManager::TimerId addTimer(
        std::chrono::milliseconds delay,
        std::function<void()> callback
    );
    bool cancelTimer(TimerManager::TimerId id);
    void sleepFor(std::chrono::milliseconds delay);

    bool cancel(int fd, Event event);
    void run();
    std::size_t pendingEventCount() const;

private:
    struct WaitState
    {
        std::optional<WaitResult> result;
    };

    struct Registration
    {
        std::uint64_t fiber_id = 0;
        std::shared_ptr<WaitState> state;
    };

    struct FdContext
    {
        Registration read;
        Registration write;
    };

    static std::uint32_t eventMask(Event event) noexcept;
    static std::uint32_t interestMask(const FdContext& context
    ) noexcept;

    void requireOwnerThread() const;
    void registerWait(
        int fd, Event event, const std::shared_ptr<WaitState>& state
    );
    WaitResult wait(
        int fd,
        Event event,
        std::optional<std::chrono::milliseconds> timeout
    );
    bool updateInterest(
        int fd, std::uint32_t old_mask, std::uint32_t new_mask
    );
    bool finish(int fd, std::uint32_t event_mask, WaitResult result);
    void waitForEvents();

    std::thread::id m_ownerThread;
    Scheduler m_scheduler;
    TimerManager m_timers;
    int m_epollFd = -1;
    std::unordered_map<int, FdContext> m_waiters;
    std::size_t m_pendingEvents = 0;
};

} // namespace sylar
