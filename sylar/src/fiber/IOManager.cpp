#include "fiber/IOManager.hpp"

#include <fcntl.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <limits>
#include <stdexcept>
#include <system_error>
#include <utility>
#include <vector>

namespace sylar
{
namespace
{

thread_local IOManager* current_io = nullptr;

class CurrentIoGuard
{
public:
    explicit CurrentIoGuard(IOManager* io)
        : m_previous(current_io)
    {
        current_io = io;
    }

    ~CurrentIoGuard()
    {
        current_io = m_previous;
    }

private:
    IOManager* m_previous;
};

} // namespace

IOManager::IOManager()
    : m_ownerThread(std::this_thread::get_id())
    , m_diagnosticsEnabled(
          []
          {
              const char* value = std::getenv("SYLAR_IO_DIAGNOSTICS");
              return value && value[0] == '1' && value[1] == '\0';
          }()
      )
    , m_epollFd(::epoll_create1(EPOLL_CLOEXEC))
{
    if (m_epollFd < 0)
    {
        throw std::system_error(
            errno,
            std::generic_category(),
            "epoll_create1"
        );
    }
    m_wakeupFd = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (m_wakeupFd < 0)
    {
        const int error = errno;
        ::close(m_epollFd);
        throw std::system_error(
            error,
            std::generic_category(),
            "eventfd"
        );
    }
    epoll_event wakeup{};
    wakeup.events = EPOLLIN;
    wakeup.data.fd = m_wakeupFd;
    if (::epoll_ctl(m_epollFd, EPOLL_CTL_ADD, m_wakeupFd, &wakeup) !=
        0)
    {
        const int error = errno;
        ::close(m_wakeupFd);
        ::close(m_epollFd);
        throw std::system_error(
            error,
            std::generic_category(),
            "epoll_ctl(eventfd)"
        );
    }
}

IOManager::~IOManager()
{
    if (m_wakeupFd >= 0)
    {
        ::close(m_wakeupFd);
    }
    if (m_epollFd >= 0)
    {
        ::close(m_epollFd);
    }
}

void IOManager::requireOwnerThread() const
{
    if (std::this_thread::get_id() != m_ownerThread)
    {
        throw std::logic_error("IOManager used from another thread");
    }
}

std::uint32_t IOManager::eventMask(Event event) noexcept
{
    return event == Event::Read ? EPOLLIN : EPOLLOUT;
}

std::uint32_t IOManager::interestMask(const FdContext& context
) noexcept
{
    return (context.read.state ? EPOLLIN : 0U) |
           (context.write.state ? EPOLLOUT : 0U);
}

Fiber::ptr IOManager::schedule(
    std::function<void()> callback, std::size_t stacksize
)
{
    requireOwnerThread();
    return m_scheduler.schedule(std::move(callback), stacksize);
}

bool IOManager::updateInterest(
    int fd, std::uint32_t old_mask, std::uint32_t new_mask
)
{
    const int operation = old_mask == 0
                              ? EPOLL_CTL_ADD
                              : (new_mask == 0 ? EPOLL_CTL_DEL
                                               : EPOLL_CTL_MOD);
    epoll_event kernel_event{};
    kernel_event.events = new_mask;
    if (new_mask & EPOLLIN)
    {
        kernel_event.events |= EPOLLRDHUP;
    }
    kernel_event.data.fd = fd;

    if (m_diagnosticsEnabled)
    {
        m_epollCtlCalls.fetch_add(1, std::memory_order_relaxed);
    }
    if (::epoll_ctl(
            m_epollFd,
            operation,
            fd,
            new_mask == 0 ? nullptr : &kernel_event
        ) == 0)
    {
        return true;
    }

    const int error = errno;
    if (operation != EPOLL_CTL_ADD &&
        (error == EBADF || error == ENOENT))
    {
        // Closing an fd removes its epoll registration in the kernel.
        return false;
    }
    throw std::system_error(
        error,
        std::generic_category(),
        "epoll_ctl"
    );
}

void IOManager::registerWait(
    int fd, Event event, const std::shared_ptr<WaitState>& state
)
{
    requireOwnerThread();
    auto* fiber = Fiber::GetCurrent();
    if (Scheduler::GetThis() != &m_scheduler || !fiber ||
        fiber->getId() == 0)
    {
        throw std::logic_error("I/O wait requires an IOManager Fiber"
        );
    }
    if (fd < 0)
    {
        throw std::invalid_argument("invalid file descriptor");
    }

    const int flags = ::fcntl(fd, F_GETFL);
    if (flags < 0)
    {
        throw std::system_error(
            errno,
            std::generic_category(),
            "fcntl(F_GETFL)"
        );
    }
    if ((flags & O_NONBLOCK) == 0)
    {
        throw std::invalid_argument(
            "I/O wait requires a nonblocking fd"
        );
    }

    auto [it, inserted] = m_waiters.try_emplace(fd);
    auto& context = it->second;
    auto& slot = event == Event::Read ? context.read : context.write;
    if (slot.state)
    {
        throw std::logic_error(
            "fd already has a waiter for this event"
        );
    }

    const auto old_mask = interestMask(context);
    try
    {
        if (!updateInterest(
                fd,
                old_mask,
                old_mask | eventMask(event)
            ))
        {
            throw std::logic_error("fd was removed from epoll");
        }
    }
    catch (...)
    {
        if (inserted)
        {
            m_waiters.erase(it);
        }
        throw;
    }

    slot.fiber_id = fiber->getId();
    slot.state = state;
    ++m_pendingEvents;
    if (m_diagnosticsEnabled)
    {
        m_waiterRegistrations.fetch_add(1, std::memory_order_relaxed);
    }
}

IOManager::WaitResult IOManager::wait(
    int fd,
    Event event,
    std::optional<std::chrono::milliseconds> timeout,
    std::stop_token cancellation
)
{
    if (timeout && timeout->count() < 0)
    {
        throw std::invalid_argument("negative I/O timeout");
    }

    if (m_stopRequested.load() || cancellation.stop_requested())
    {
        return WaitResult::Cancelled;
    }
    if (timeout && *timeout == std::chrono::milliseconds::zero())
    {
        return WaitResult::TimedOut;
    }
    auto state = std::make_shared<WaitState>();
    registerWait(fd, event, state);
    TimerManager::TimerId timer_id = 0;
    if (timeout)
    {
        try
        {
            timer_id = m_timers.addAfter(
                *timeout,
                [this, fd, event, state] {
                    finishIfCurrent(
                        fd,
                        event,
                        state,
                        WaitResult::TimedOut
                    );
                }
            );
        }
        catch (...)
        {
            cancel(fd, event);
            throw;
        }
    }
    std::optional<std::stop_callback<std::function<void()>>>
        stop_callback;
    try
    {
        if (cancellation.stop_possible())
        {
            stop_callback.emplace(
                cancellation,
                [this, fd, event, state]
                {
                    try
                    {
                        post(
                            [this, fd, event, state] {
                                finishIfCurrent(
                                    fd,
                                    event,
                                    state,
                                    WaitResult::Cancelled
                                );
                            }
                        );
                    }
                    catch (const std::exception&)
                    {
                        // Shutdown cancels remaining waits; a failed
                        // wakeup must not escape a stop callback.
                    }
                }
            );
        }
    }
    catch (...)
    {
        if (timer_id != 0)
        {
            m_timers.cancel(timer_id);
        }
        cancel(fd, event);
        throw;
    }
    Fiber::YieldToHold();
    stop_callback.reset();
    if (timer_id != 0)
    {
        m_timers.cancel(timer_id);
    }

    if (!state->result)
    {
        // An unrelated wake-up must not leave a stale epoll entry.
        cancel(fd, event);
        throw std::logic_error(
            "I/O Fiber resumed without an I/O event"
        );
    }
    return *state->result;
}

bool IOManager::finishIfCurrent(
    int fd,
    Event event,
    const std::shared_ptr<WaitState>& state,
    WaitResult result
)
{
    const auto found = m_waiters.find(fd);
    if (found == m_waiters.end())
    {
        return false;
    }
    const auto& slot = event == Event::Read ? found->second.read
                                            : found->second.write;
    if (slot.state != state)
    {
        return false;
    }
    return finish(fd, eventMask(event), result);
}

IOManager::WaitResult IOManager::waitReadable(int fd)
{
    return wait(fd, Event::Read, std::nullopt);
}

IOManager::WaitResult IOManager::waitWritable(int fd)
{
    return wait(fd, Event::Write, std::nullopt);
}

IOManager::WaitResult
IOManager::waitReadable(int fd, std::chrono::milliseconds timeout)
{
    return wait(fd, Event::Read, timeout);
}

IOManager::WaitResult
IOManager::waitWritable(int fd, std::chrono::milliseconds timeout)
{
    return wait(fd, Event::Write, timeout);
}

IOManager::WaitResult IOManager::waitReadable(
    int fd,
    std::chrono::milliseconds timeout,
    const RequestContext& context
)
{
    if (const auto remaining = context.remaining())
    {
        timeout = std::min(timeout, *remaining);
    }
    return wait(fd, Event::Read, timeout, context.token());
}

IOManager::WaitResult IOManager::waitWritable(
    int fd,
    std::chrono::milliseconds timeout,
    const RequestContext& context
)
{
    if (const auto remaining = context.remaining())
    {
        timeout = std::min(timeout, *remaining);
    }
    return wait(fd, Event::Write, timeout, context.token());
}

IOManager::WaitResult
IOManager::waitReadable(int fd, const RequestContext& context)
{
    return wait(
        fd,
        Event::Read,
        context.remaining(),
        context.token()
    );
}

IOManager::WaitResult
IOManager::waitWritable(int fd, const RequestContext& context)
{
    return wait(
        fd,
        Event::Write,
        context.remaining(),
        context.token()
    );
}

TimerManager::TimerId IOManager::addTimer(
    std::chrono::milliseconds delay, std::function<void()> callback
)
{
    requireOwnerThread();
    return m_timers.addAfter(delay, std::move(callback));
}

bool IOManager::cancelTimer(TimerManager::TimerId id)
{
    requireOwnerThread();
    return m_timers.cancel(id);
}

void IOManager::sleepFor(std::chrono::milliseconds delay)
{
    requireOwnerThread();
    auto* fiber = Fiber::GetCurrent();
    if (Scheduler::GetThis() != &m_scheduler || !fiber ||
        fiber->getId() == 0)
    {
        throw std::logic_error("sleep requires an IOManager Fiber");
    }
    const auto fiber_id = fiber->getId();
    m_timers.addAfter(
        delay,
        [this, fiber_id] { m_scheduler.wake(fiber_id); }
    );
    Fiber::YieldToHold();
}

bool IOManager::finish(
    int fd, std::uint32_t event_mask, WaitResult result
)
{
    auto it = m_waiters.find(fd);
    if (it == m_waiters.end())
    {
        return false;
    }

    auto& context = it->second;
    const auto old_mask = interestMask(context);
    auto completed_mask = old_mask & event_mask;
    if (completed_mask == 0)
    {
        return false;
    }

    auto new_mask = old_mask & ~completed_mask;
    if (!updateInterest(fd, old_mask, new_mask))
    {
        // The fd was closed; no remaining waiter can receive an
        // event.
        completed_mask = old_mask;
        new_mask = 0;
        result = WaitResult::Cancelled;
    }

    Registration read;
    Registration write;
    if (completed_mask & EPOLLIN)
    {
        read = std::move(context.read);
        context.read = {};
        --m_pendingEvents;
    }
    if (completed_mask & EPOLLOUT)
    {
        write = std::move(context.write);
        context.write = {};
        --m_pendingEvents;
    }
    if (new_mask == 0)
    {
        m_waiters.erase(it);
    }

    if (read.state)
    {
        read.state->result = result;
        m_scheduler.wake(read.fiber_id);
    }
    if (write.state)
    {
        write.state->result = result;
        m_scheduler.wake(write.fiber_id);
    }
    return true;
}

bool IOManager::cancel(int fd, Event event)
{
    requireOwnerThread();
    return finish(fd, eventMask(event), WaitResult::Cancelled);
}

void IOManager::waitForEvents()
{
    std::array<epoll_event, 64> events{};
    int timeout = -1;
    if (const auto next = m_timers.nextTimeout())
    {
        timeout = next->count() > std::numeric_limits<int>::max()
                      ? std::numeric_limits<int>::max()
                      : static_cast<int>(next->count());
    }
    const auto
        wait_start = m_diagnosticsEnabled
                         ? std::chrono::steady_clock::now()
                         : std::chrono::steady_clock::time_point{};
    int count;
    do
    {
        count = ::epoll_wait(
            m_epollFd,
            events.data(),
            static_cast<int>(events.size()),
            timeout
        );
    } while (count < 0 && errno == EINTR);

    if (count < 0)
    {
        throw std::system_error(
            errno,
            std::generic_category(),
            "epoll_wait"
        );
    }
    if (m_diagnosticsEnabled)
    {
        m_epollWaitCalls.fetch_add(1, std::memory_order_relaxed);
        m_epollReadyEvents.fetch_add(
            static_cast<std::uint64_t>(count),
            std::memory_order_relaxed
        );
        m_epollWaitNs.fetch_add(
            static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - wait_start
                )
                    .count()
            ),
            std::memory_order_relaxed
        );
    }

    for (int index = 0; index < count; ++index)
    {
        if (events[index].data.fd == m_wakeupFd)
        {
            if (m_diagnosticsEnabled)
            {
                m_eventfdWakeups.fetch_add(
                    1,
                    std::memory_order_relaxed
                );
            }
            std::uint64_t counter;
            while (::read(m_wakeupFd, &counter, sizeof(counter)) ==
                   sizeof(counter))
            {
            }
            continue;
        }
        const auto flags = events[index].events;
        std::uint32_t completed = 0;
        if (flags &
            (EPOLLIN | EPOLLPRI | EPOLLRDHUP | EPOLLERR | EPOLLHUP))
        {
            completed |= EPOLLIN;
        }
        if (flags & (EPOLLOUT | EPOLLERR | EPOLLHUP))
        {
            completed |= EPOLLOUT;
        }
        finish(events[index].data.fd, completed, WaitResult::Ready);
    }
}

void IOManager::signalWakeup()
{
    const std::uint64_t one = 1;
    if (::write(m_wakeupFd, &one, sizeof(one)) < 0 && errno != EAGAIN)
    {
        throw std::system_error(
            errno,
            std::generic_category(),
            "eventfd write"
        );
    }
}

void IOManager::post(std::function<void()> callback)
{
    if (!callback)
    {
        throw std::invalid_argument("empty posted task");
    }
    {
        std::lock_guard lock(m_postMutex);
        if (m_stopRequested.load())
        {
            throw std::logic_error("IOManager is stopping");
        }
        m_posts.push_back(std::move(callback));
        if (m_diagnosticsEnabled)
        {
            m_postCount.fetch_add(1, std::memory_order_relaxed);
            m_postQueue.fetch_add(1, std::memory_order_relaxed);
        }
    }
    signalWakeup();
}

void IOManager::requestStop()
{
    {
        std::lock_guard lock(m_postMutex);
        m_stopRequested.store(true);
    }
    signalWakeup();
}

void IOManager::drainPosts()
{
    std::deque<std::function<void()>> pending;
    {
        std::lock_guard lock(m_postMutex);
        pending.swap(m_posts);
        if (m_diagnosticsEnabled)
        {
            m_postQueue.fetch_sub(
                pending.size(),
                std::memory_order_relaxed
            );
        }
    }
    for (auto& callback : pending)
    {
        m_scheduler.schedule(std::move(callback));
    }
}

void IOManager::cancelAllWaits()
{
    std::vector<int> fds;
    fds.reserve(m_waiters.size());
    for (const auto& [fd, context] : m_waiters)
    {
        (void)context;
        fds.push_back(fd);
    }
    for (int fd : fds)
    {
        finish(fd, EPOLLIN | EPOLLOUT, WaitResult::Cancelled);
    }
}

void IOManager::runForever()
{
    requireOwnerThread();
    CurrentIoGuard guard(this);
    for (;;)
    {
        drainPosts();
        m_scheduler.run();
        if (m_stopRequested.load())
        {
            // A producer may have posted just before requestStop().
            drainPosts();
            m_scheduler.run();
            cancelAllWaits();
            m_timers.clear();
            m_scheduler.run();
            return;
        }
        for (auto& callback : m_timers.takeExpired())
        {
            m_scheduler.schedule(std::move(callback));
        }
        if (m_scheduler.readyCount() != 0)
        {
            continue;
        }
        waitForEvents();
    }
}

void IOManager::run()
{
    requireOwnerThread();
    CurrentIoGuard guard(this);
    for (;;)
    {
        drainPosts();
        m_scheduler.run();
        for (auto& callback : m_timers.takeExpired())
        {
            m_scheduler.schedule(std::move(callback));
        }
        if (m_scheduler.readyCount() != 0)
        {
            continue;
        }
        if (m_pendingEvents == 0 && m_timers.empty())
        {
            return;
        }
        waitForEvents();
    }
}

IOManager::Diagnostics IOManager::diagnostics() const noexcept
{
    return {
        .enabled = m_diagnosticsEnabled,
        .posts = m_postCount.load(std::memory_order_relaxed),
        .post_queue = m_postQueue.load(std::memory_order_relaxed),
        .waiter_registrations = m_waiterRegistrations.load(
            std::memory_order_relaxed
        ),
        .epoll_ctl_calls = m_epollCtlCalls.load(
            std::memory_order_relaxed
        ),
        .epoll_wait_calls = m_epollWaitCalls.load(
            std::memory_order_relaxed
        ),
        .epoll_wait_ns = m_epollWaitNs.load(std::memory_order_relaxed
        ),
        .epoll_ready_events = m_epollReadyEvents.load(
            std::memory_order_relaxed
        ),
        .eventfd_wakeups = m_eventfdWakeups.load(
            std::memory_order_relaxed
        ),
    };
}

IOManager* IOManager::GetThis() noexcept
{
    return current_io;
}

std::size_t IOManager::pendingEventCount() const
{
    requireOwnerThread();
    return m_pendingEvents;
}

} // namespace sylar
