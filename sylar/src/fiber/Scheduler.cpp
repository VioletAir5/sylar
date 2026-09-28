#include "fiber/Scheduler.hpp"

#include <algorithm>
#include <exception>
#include <stdexcept>
#include <utility>

namespace sylar
{
namespace
{

thread_local Scheduler* current_scheduler = nullptr;

} // namespace

Scheduler::Scheduler()
    : m_ownerThread(std::this_thread::get_id())
{
}

Scheduler::~Scheduler()
{
    for (const auto& fiber : m_ready)
    {
        fiber->m_scheduler = nullptr;
    }
    for (const auto& [id, fiber] : m_held)
    {
        (void)id;
        fiber->m_scheduler = nullptr;
    }
}

void Scheduler::requireOwnerThread() const
{
    if (std::this_thread::get_id() != m_ownerThread)
    {
        throw std::logic_error("Scheduler used from another thread");
    }
}

void Scheduler::schedule(Fiber::ptr fiber)
{
    requireOwnerThread();
    if (!fiber || fiber->getId() == 0 || fiber->m_useCaller ||
        fiber->m_scheduler ||
        (fiber->getState() != Fiber::INIT &&
         fiber->getState() != Fiber::READY))
    {
        throw std::invalid_argument("Fiber cannot be scheduled");
    }

    m_ready.push_back(fiber);
    fiber->m_scheduler = this;
}

Fiber::ptr Scheduler::schedule(
    std::function<void()> callback, std::size_t stacksize
)
{
    requireOwnerThread();
    auto fiber = std::make_shared<Fiber>(
        std::move(callback),
        stacksize
    );
    schedule(fiber);
    return fiber;
}

bool Scheduler::wake(std::uint64_t fiber_id)
{
    requireOwnerThread();
    auto it = m_held.find(fiber_id);
    if (it == m_held.end())
    {
        return false;
    }

    m_ready.push_back(it->second);
    m_held.erase(it);
    return true;
}

bool Scheduler::cancel(std::uint64_t fiber_id)
{
    requireOwnerThread();
    auto held = m_held.find(fiber_id);
    if (held != m_held.end())
    {
        held->second->m_scheduler = nullptr;
        m_held.erase(held);
        return true;
    }

    auto ready = std::find_if(
        m_ready.begin(),
        m_ready.end(),
        [fiber_id](const Fiber::ptr& fiber)
        { return fiber->getId() == fiber_id; }
    );
    if (ready == m_ready.end())
    {
        return false;
    }

    (*ready)->m_scheduler = nullptr;
    m_ready.erase(ready);
    return true;
}

void Scheduler::run()
{
    requireOwnerThread();
    if (m_running)
    {
        throw std::logic_error("Scheduler cannot run recursively");
    }

    Fiber::GetThis();
    m_running = true;
    auto* previous_scheduler = current_scheduler;
    current_scheduler = this;
    std::exception_ptr first_error;

    try
    {
        while (!m_ready.empty())
        {
            auto fiber = std::move(m_ready.front());
            m_ready.pop_front();

            try
            {
                fiber->resume();

                switch (fiber->getState())
                {
                case Fiber::READY:
                    m_ready.push_back(fiber);
                    break;
                case Fiber::HOLD:
                    m_held.emplace(fiber->getId(), fiber);
                    break;
                case Fiber::TERM:
                    fiber->m_scheduler = nullptr;
                    break;
                case Fiber::EXCEPT:
                    fiber->m_scheduler = nullptr;
                    if (!first_error)
                    {
                        try
                        {
                            fiber->rethrowIfFailed();
                        }
                        catch (...)
                        {
                            first_error = std::current_exception();
                        }
                    }
                    break;
                default:
                    throw std::logic_error(
                        "Fiber returned in an invalid state"
                    );
                }
            }
            catch (...)
            {
                fiber->m_scheduler = nullptr;
                throw;
            }
        }
    }
    catch (...)
    {
        current_scheduler = previous_scheduler;
        m_running = false;
        throw;
    }

    current_scheduler = previous_scheduler;
    m_running = false;
    if (first_error)
    {
        std::rethrow_exception(first_error);
    }
}

Scheduler* Scheduler::GetThis() noexcept
{
    return current_scheduler;
}

std::size_t Scheduler::readyCount() const
{
    requireOwnerThread();
    return m_ready.size();
}

std::size_t Scheduler::heldCount() const
{
    requireOwnerThread();
    return m_held.size();
}

} // namespace sylar
