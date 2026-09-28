#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <thread>
#include <unordered_map>

#include "fiber/Fiber.hpp"

namespace sylar
{

// Cooperative scheduler. All methods must be called on its owner
// thread.
class Scheduler
{
public:
    Scheduler();
    ~Scheduler();

    Scheduler(const Scheduler&) = delete;
    Scheduler& operator=(const Scheduler&) = delete;
    Scheduler(Scheduler&&) = delete;
    Scheduler& operator=(Scheduler&&) = delete;

    void schedule(Fiber::ptr fiber);
    Fiber::ptr schedule(
        std::function<void()> callback, std::size_t stacksize = 0
    );

    // A held fiber stays owned by the scheduler until wake or cancel.
    bool wake(std::uint64_t fiber_id);
    bool cancel(std::uint64_t fiber_id);
    void run();

    static Scheduler* GetThis() noexcept;
    std::size_t readyCount() const;
    std::size_t heldCount() const;

private:
    void requireOwnerThread() const;

    std::thread::id m_ownerThread;
    bool m_running = false;
    std::deque<Fiber::ptr> m_ready;
    std::unordered_map<std::uint64_t, Fiber::ptr> m_held;
};

} // namespace sylar
