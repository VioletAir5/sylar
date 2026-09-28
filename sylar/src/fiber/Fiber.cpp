#include "fiber/Fiber.hpp"

#include <atomic>
#include <stdexcept>
#include <utility>

#include <boost/context/detail/exception.hpp>
#include <boost/context/fixedsize_stack.hpp>
#include <boost/context/stack_traits.hpp>

#include "config/Config.hpp"

namespace sylar
{
namespace
{

std::atomic<std::uint64_t> next_fiber_id{0};
std::atomic<std::uint64_t> fiber_count{0};
thread_local Fiber* current_fiber = nullptr;
thread_local Fiber::ptr main_fiber;

std::size_t defaultStackSize()
{
    static auto configured = Config::Lookup<std::uint32_t>(
        "fiber.stack_size",
        128 * 1024,
        "Default Fiber stack size in bytes"
    );
    if (!configured)
    {
        throw std::logic_error("fiber.stack_size has another type");
    }
    return configured->getValue();
}

} // namespace

Fiber::Fiber()
    : m_state(EXEC)
{
    ++fiber_count;
}

Fiber::Fiber(
    std::function<void()> cb, std::size_t stacksize, bool use_caller
)
    : m_id(++next_fiber_id)
    , m_stacksize(stacksize == 0 ? defaultStackSize() : stacksize)
    , m_useCaller(use_caller)
    , m_cb(std::move(cb))
{
    if (!m_cb)
    {
        throw std::invalid_argument("Fiber callback is empty");
    }
    if (m_stacksize < boost::context::stack_traits::minimum_size())
    {
        throw std::invalid_argument("Fiber stack is too small");
    }

    m_context = makeContext();
    ++fiber_count;
}

Fiber::~Fiber()
{
    if (current_fiber == this)
    {
        current_fiber = nullptr;
    }
    --fiber_count;
}

boost::context::fiber Fiber::makeContext()
{
    return boost::context::fiber{
        std::allocator_arg,
        boost::context::fixedsize_stack{m_stacksize},
        [this](boost::context::fiber&& caller
        ) -> boost::context::fiber
        {
            m_caller = std::move(caller);

            try
            {
                m_cb();
                m_state = TERM;
            }
            catch (const boost::context::detail::forced_unwind&)
            {
                throw;
            }
            catch (...)
            {
                m_error = std::current_exception();
                m_state = EXCEPT;
            }

            m_cb = {};
            return std::move(m_caller);
        }};
}

void Fiber::reset(std::function<void()> cb)
{
    if (m_id == 0 || m_scheduler ||
        (m_state != INIT && m_state != TERM && m_state != EXCEPT))
    {
        throw std::logic_error("Fiber cannot be reset in this state");
    }
    if (!cb)
    {
        throw std::invalid_argument("Fiber callback is empty");
    }

    auto replacement = makeContext();
    m_context = std::move(replacement);
    m_cb = std::move(cb);
    m_error = nullptr;
    m_state = INIT;
}

void Fiber::resume()
{
    if (m_id == 0 ||
        (m_state != INIT && m_state != READY && m_state != HOLD))
    {
        throw std::logic_error("Fiber is not resumable");
    }

    auto previous = GetThis();
    SetThis(this);
    m_state = EXEC;

    try
    {
        m_context = std::move(m_context).resume();
    }
    catch (...)
    {
        SetThis(previous.get());
        throw;
    }

    SetThis(previous.get());
}

void Fiber::suspend()
{
    if (current_fiber != this || m_id == 0 ||
        (m_state != EXEC && m_state != READY && m_state != HOLD))
    {
        throw std::logic_error("Fiber is not running");
    }

    if (m_state == EXEC)
    {
        m_state = HOLD;
    }

    m_caller = std::move(m_caller).resume();
}

void Fiber::swapIn()
{
    if (m_useCaller)
    {
        throw std::logic_error("Caller Fiber requires call()");
    }
    if (m_scheduler)
    {
        throw std::logic_error("Scheduler owns this Fiber");
    }
    resume();
}

void Fiber::swapOut()
{
    if (m_useCaller)
    {
        throw std::logic_error("Caller Fiber requires back()");
    }
    suspend();
}

void Fiber::call()
{
    if (!m_useCaller)
    {
        throw std::logic_error("Scheduled Fiber requires swapIn()");
    }
    resume();
}

void Fiber::back()
{
    if (!m_useCaller)
    {
        throw std::logic_error("Scheduled Fiber requires swapOut()");
    }
    suspend();
}

std::uint64_t Fiber::getId() const noexcept
{
    return m_id;
}

std::size_t Fiber::getStackSize() const noexcept
{
    return m_stacksize;
}

Fiber::State Fiber::getState() const noexcept
{
    return m_state;
}

void Fiber::rethrowIfFailed() const
{
    if (m_error)
    {
        std::rethrow_exception(m_error);
    }
}

void Fiber::SetThis(Fiber* fiber) noexcept
{
    current_fiber = fiber;
}

Fiber::ptr Fiber::GetThis()
{
    if (current_fiber)
    {
        return current_fiber->shared_from_this();
    }
    if (main_fiber)
    {
        SetThis(main_fiber.get());
        return main_fiber;
    }

    auto fiber = ptr(new Fiber);
    main_fiber = fiber;
    SetThis(fiber.get());
    return fiber;
}

Fiber* Fiber::GetCurrent() noexcept
{
    return current_fiber;
}

void Fiber::YieldToReady()
{
    auto* fiber = current_fiber;
    if (!fiber || fiber->m_id == 0 || fiber->m_state != EXEC)
    {
        throw std::logic_error("Only a running Fiber can yield");
    }

    fiber->m_state = READY;
    if (fiber->m_useCaller)
    {
        fiber->back();
    }
    else
    {
        fiber->swapOut();
    }
}

void Fiber::YieldToHold()
{
    auto* fiber = current_fiber;
    if (!fiber || fiber->m_id == 0 || fiber->m_state != EXEC)
    {
        throw std::logic_error("Only a running Fiber can yield");
    }

    fiber->m_state = HOLD;
    if (fiber->m_useCaller)
    {
        fiber->back();
    }
    else
    {
        fiber->swapOut();
    }
}

std::uint64_t Fiber::TotalFibers() noexcept
{
    return fiber_count.load();
}

std::uint64_t Fiber::GetFiberId() noexcept
{
    return current_fiber ? current_fiber->m_id : 0;
}

} // namespace sylar
