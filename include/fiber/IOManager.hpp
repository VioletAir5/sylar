#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <thread>
#include <unordered_map>

#include "fiber/RequestContext.hpp"
#include "fiber/Scheduler.hpp"
#include "fiber/TimerManager.hpp"

namespace sylar
{

// Linux epoll integration for one owner thread and one Scheduler.
class IOManager
{
public:
    struct Diagnostics
    {
        bool enabled = false;
        std::uint64_t posts = 0;
        std::uint64_t post_queue = 0;
        std::uint64_t waiter_registrations = 0;
        std::uint64_t epoll_ctl_calls = 0;
        std::uint64_t epoll_wait_calls = 0;
        std::uint64_t epoll_wait_ns = 0;
        std::uint64_t epoll_ready_events = 0;
        std::uint64_t eventfd_wakeups = 0;
    };

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
    WaitResult waitReadable(
        int fd,
        std::chrono::milliseconds timeout,
        const RequestContext& context
    );
    WaitResult waitWritable(
        int fd,
        std::chrono::milliseconds timeout,
        const RequestContext& context
    );
    WaitResult waitReadable(int fd, const RequestContext& context);
    WaitResult waitWritable(int fd, const RequestContext& context);

    TimerManager::TimerId addTimer(
        std::chrono::milliseconds delay,
        std::function<void()> callback
    );
    bool cancelTimer(TimerManager::TimerId id);
    void sleepFor(std::chrono::milliseconds delay);

    bool cancel(int fd, Event event);
    // Thread-safe submission and stop signal for runForever().
    void post(std::function<void()> callback);
    void requestStop();
    void runForever();
    void run();
    std::size_t pendingEventCount() const;
    Diagnostics diagnostics() const noexcept;
    static IOManager* GetThis() noexcept;

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
        std::optional<std::chrono::milliseconds> timeout,
        std::stop_token cancellation = {}
    );
    bool finishIfCurrent(
        int fd,
        Event event,
        const std::shared_ptr<WaitState>& state,
        WaitResult result
    );
    bool updateInterest(
        int fd, std::uint32_t old_mask, std::uint32_t new_mask
    );
    bool finish(int fd, std::uint32_t event_mask, WaitResult result);
    void waitForEvents();
    void signalWakeup();
    void drainPosts();
    void cancelAllWaits();

    std::thread::id m_ownerThread;
    bool m_diagnosticsEnabled = false;
    TimerManager m_timers;
    Scheduler m_scheduler;
    int m_epollFd = -1;
    int m_wakeupFd = -1;
    std::mutex m_postMutex;
    std::deque<std::function<void()>> m_posts;
    std::atomic<bool> m_stopRequested{false};
    std::unordered_map<int, FdContext> m_waiters;
    std::size_t m_pendingEvents = 0;
    std::atomic_uint64_t m_postCount{0};
    std::atomic_uint64_t m_postQueue{0};
    std::atomic_uint64_t m_waiterRegistrations{0};
    std::atomic_uint64_t m_epollCtlCalls{0};
    std::atomic_uint64_t m_epollWaitCalls{0};
    std::atomic_uint64_t m_epollWaitNs{0};
    std::atomic_uint64_t m_epollReadyEvents{0};
    std::atomic_uint64_t m_eventfdWakeups{0};
};

} // namespace sylar
