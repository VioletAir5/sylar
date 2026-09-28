#pragma once

#include <cerrno>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <system_error>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include "fiber/IOManager.hpp"

namespace sylar
{

// Runs blocking work on dedicated threads while the caller Fiber
// waits through IOManager's epoll loop.
class BlockingExecutor
{
public:
    explicit BlockingExecutor(std::size_t threads);
    ~BlockingExecutor();

    BlockingExecutor(const BlockingExecutor&) = delete;
    BlockingExecutor& operator=(const BlockingExecutor&) = delete;

    template <class Function>
    auto await(IOManager& io, Function&& operation)
        -> std::invoke_result_t<std::decay_t<Function>&>
    {
        using Result = std::invoke_result_t<std::decay_t<Function>&>;
        auto signal = std::make_shared<EventSignal>();
        auto task = std::make_shared<std::packaged_task<Result()>>(
            std::forward<Function>(operation)
        );
        auto future = task->get_future();
        enqueue(
            [signal, task]
            {
                (*task)();
                signal->notify();
            }
        );
        const auto result = io.waitReadable(signal->fd());
        if (result != IOManager::WaitResult::Ready)
        {
            throw std::system_error(
                ECANCELED,
                std::generic_category(),
                "blocking task wait"
            );
        }
        return future.get();
    }

private:
    class EventSignal
    {
    public:
        EventSignal();
        ~EventSignal();
        EventSignal(const EventSignal&) = delete;
        EventSignal& operator=(const EventSignal&) = delete;

        int fd() const noexcept;
        void notify() const noexcept;

    private:
        int m_fd = -1;
    };

    void enqueue(std::function<void()> task);
    void worker();

    std::mutex m_mutex;
    std::condition_variable m_condition;
    std::deque<std::function<void()>> m_queue;
    std::vector<std::thread> m_threads;
    bool m_stopping = false;
};

} // namespace sylar