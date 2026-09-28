#include "fiber/TimerManager.hpp"

#include <limits>
#include <stdexcept>
#include <utility>

namespace sylar
{

TimerManager::TimerManager()
    : m_ownerThread(std::this_thread::get_id())
{
}

void TimerManager::requireOwnerThread() const
{
    if (std::this_thread::get_id() != m_ownerThread)
    {
        throw std::logic_error("TimerManager used from another thread"
        );
    }
}

TimerManager::TimerId TimerManager::addAfter(
    std::chrono::milliseconds delay, std::function<void()> callback
)
{
    requireOwnerThread();
    if (delay.count() < 0 || !callback)
    {
        throw std::invalid_argument("invalid timer");
    }
    if (m_nextId == std::numeric_limits<TimerId>::max())
    {
        throw std::overflow_error("timer id exhausted");
    }

    const TimerId id = m_nextId++;
    const auto deadline = Clock::now() + delay;
    auto it = m_timers.emplace(
        deadline,
        Entry{id, std::move(callback)}
    );
    try
    {
        m_byId.emplace(id, it);
    }
    catch (...)
    {
        m_timers.erase(it);
        throw;
    }
    return id;
}

bool TimerManager::cancel(TimerId id)
{
    requireOwnerThread();
    auto it = m_byId.find(id);
    if (it == m_byId.end())
    {
        return false;
    }
    m_timers.erase(it->second);
    m_byId.erase(it);
    return true;
}

void TimerManager::clear()
{
    requireOwnerThread();
    m_byId.clear();
    m_timers.clear();
}

std::optional<std::chrono::milliseconds>
TimerManager::nextTimeout() const
{
    requireOwnerThread();
    if (m_timers.empty())
    {
        return std::nullopt;
    }
    const auto now = Clock::now();
    if (m_timers.begin()->first <= now)
    {
        return std::chrono::milliseconds::zero();
    }
    return std::chrono::ceil<std::chrono::milliseconds>(
        m_timers.begin()->first - now
    );
}

std::vector<std::function<void()>> TimerManager::takeExpired()
{
    requireOwnerThread();
    std::vector<std::function<void()>> callbacks;
    const auto now = Clock::now();
    while (!m_timers.empty() && m_timers.begin()->first <= now)
    {
        auto it = m_timers.begin();
        callbacks.push_back(std::move(it->second.callback));
        m_byId.erase(it->second.id);
        m_timers.erase(it);
    }
    return callbacks;
}

bool TimerManager::empty() const
{
    requireOwnerThread();
    return m_timers.empty();
}

} // namespace sylar
