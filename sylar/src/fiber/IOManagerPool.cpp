#include "fiber/IOManagerPool.hpp"

#include <exception>
#include <future>
#include <stdexcept>
#include <thread>
#include <utility>

namespace sylar
{

struct IOManagerPool::Worker
{
    std::mutex mutex;
    IOManager* io = nullptr;
    std::exception_ptr error;
    std::thread thread;
};

IOManagerPool::IOManagerPool(std::size_t workers)
{
    if (workers == 0)
    {
        throw std::invalid_argument("worker count must be positive");
    }
    try
    {
        for (std::size_t index = 0; index < workers; ++index)
        {
            auto worker = std::make_unique<Worker>();
            auto* state = worker.get();
            m_workers.push_back(std::move(worker));

            std::promise<void> ready;
            auto future = ready.get_future();
            state->thread = std::thread(
                [state, signal = std::move(ready)]() mutable
                {
                    try
                    {
                        IOManager io;
                        {
                            std::lock_guard lock(state->mutex);
                            state->io = &io;
                        }
                        signal.set_value();
                        try
                        {
                            io.runForever();
                        }
                        catch (...)
                        {
                            std::lock_guard lock(state->mutex);
                            state->error = std::current_exception();
                        }
                        {
                            std::lock_guard lock(state->mutex);
                            state->io = nullptr;
                        }
                    }
                    catch (...)
                    {
                        signal.set_exception(std::current_exception()
                        );
                    }
                }
            );
            future.get();
        }
    }
    catch (...)
    {
        try
        {
            stop();
        }
        catch (...)
        {
        }
        throw;
    }
}

IOManagerPool::~IOManagerPool()
{
    try
    {
        stop();
    }
    catch (...)
    {
    }
}

void IOManagerPool::post(std::function<void(IOManager&)> callback)
{
    if (!callback)
    {
        throw std::invalid_argument("empty pool task");
    }
    std::lock_guard pool_lock(m_mutex);
    if (m_stopped)
    {
        throw std::logic_error("IOManagerPool is stopped");
    }
    auto& worker = *m_workers[m_next];
    m_next = (m_next + 1) % m_workers.size();

    std::lock_guard worker_lock(worker.mutex);
    if (!worker.io)
    {
        throw std::runtime_error("IOManager worker exited");
    }
    auto* io = worker.io;
    io->post([io, callback = std::move(callback)]() mutable
             { callback(*io); });
}

void IOManagerPool::stop()
{
    std::exception_ptr first_error;
    {
        std::lock_guard pool_lock(m_mutex);
        if (m_stopped)
        {
            return;
        }
        m_stopped = true;
        for (auto& worker : m_workers)
        {
            std::lock_guard worker_lock(worker->mutex);
            if (worker->io)
            {
                try
                {
                    worker->io->requestStop();
                }
                catch (...)
                {
                    if (!first_error)
                    {
                        first_error = std::current_exception();
                    }
                }
            }
        }
    }

    for (auto& worker : m_workers)
    {
        if (worker->thread.joinable())
        {
            worker->thread.join();
        }
    }
    for (auto& worker : m_workers)
    {
        if (worker->error && !first_error)
        {
            first_error = worker->error;
        }
    }
    if (first_error)
    {
        std::rethrow_exception(first_error);
    }
}

std::size_t IOManagerPool::size() const noexcept
{
    return m_workers.size();
}

} // namespace sylar
