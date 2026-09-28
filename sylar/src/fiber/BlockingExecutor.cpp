#include "fiber/BlockingExecutor.hpp"

#include <sys/eventfd.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace sylar
{

BlockingExecutor::EventSignal::EventSignal()
    : m_fd(::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC))
{
    if (m_fd < 0)
    {
        throw std::system_error(
            errno,
            std::generic_category(),
            "eventfd"
        );
    }
}

BlockingExecutor::EventSignal::~EventSignal()
{
    ::close(m_fd);
}

int BlockingExecutor::EventSignal::fd() const noexcept
{
    return m_fd;
}

void BlockingExecutor::EventSignal::notify() const noexcept
{
    const std::uint64_t one = 1;
    for (;;)
    {
        if (::write(m_fd, &one, sizeof(one)) == sizeof(one))
        {
            return;
        }
        if (errno != EINTR)
        {
            return;
        }
    }
}

BlockingExecutor::BlockingExecutor(std::size_t threads)
{
    if (threads == 0 || threads > 128)
    {
        throw std::invalid_argument(
            "invalid blocking executor thread count"
        );
    }
    try
    {
        m_threads.reserve(threads);
        for (std::size_t i = 0; i < threads; ++i)
        {
            m_threads.emplace_back([this] { worker(); });
        }
    }
    catch (...)
    {
        {
            std::lock_guard lock(m_mutex);
            m_stopping = true;
        }
        m_condition.notify_all();
        for (auto& thread : m_threads)
        {
            thread.join();
        }
        throw;
    }
}

BlockingExecutor::~BlockingExecutor()
{
    {
        std::lock_guard lock(m_mutex);
        m_stopping = true;
    }
    m_condition.notify_all();
    for (auto& thread : m_threads)
    {
        thread.join();
    }
}

void BlockingExecutor::enqueue(std::function<void()> task)
{
    {
        std::lock_guard lock(m_mutex);
        if (m_stopping)
        {
            throw std::logic_error("blocking executor is stopping");
        }
        m_queue.push_back(std::move(task));
    }
    m_condition.notify_one();
}

void BlockingExecutor::worker()
{
    for (;;)
    {
        std::function<void()> task;
        {
            std::unique_lock lock(m_mutex);
            m_condition.wait(
                lock,
                [this] { return m_stopping || !m_queue.empty(); }
            );
            if (m_queue.empty() && m_stopping)
            {
                return;
            }
            task = std::move(m_queue.front());
            m_queue.pop_front();
        }
        task();
    }
}

} // namespace sylar