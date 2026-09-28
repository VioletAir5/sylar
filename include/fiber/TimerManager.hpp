#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <thread>
#include <unordered_map>
#include <vector>

namespace sylar
{

// Single-thread timer queue. Callbacks are returned for the owner to
// run.
class TimerManager
{
public:
    using Clock = std::chrono::steady_clock;
    using TimerId = std::uint64_t;

    TimerManager();

    TimerId addAfter(
        std::chrono::milliseconds delay,
        std::function<void()> callback
    );
    bool cancel(TimerId id);
    void clear();
    std::optional<std::chrono::milliseconds> nextTimeout() const;
    std::vector<std::function<void()>> takeExpired();
    bool empty() const;

private:
    struct Entry
    {
        TimerId id;
        std::function<void()> callback;
    };

    using Timers = std::multimap<Clock::time_point, Entry>;

    void requireOwnerThread() const;

    std::thread::id m_ownerThread;
    TimerId m_nextId = 1;
    Timers m_timers;
    std::unordered_map<TimerId, Timers::iterator> m_byId;
};

} // namespace sylar
