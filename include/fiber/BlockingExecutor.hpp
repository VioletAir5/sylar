#pragma once

#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <system_error>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include "fiber/IOManager.hpp"
#include "fiber/RequestContext.hpp"

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
        return awaitImpl(
            io,
            nullptr,
            std::forward<Function>(operation)
        );
    }

    template <class Function>
    auto await(
        IOManager& io,
        const RequestContext& context,
        Function&& operation
    ) -> std::invoke_result_t<std::decay_t<Function>&>
    {
        return awaitImpl(
            io,
            &context,
            std::forward<Function>(operation)
        );
    }

private:
    template <class Function>
    auto awaitImpl(
        IOManager& io,
        const RequestContext* context,
        Function&& operation
    ) -> std::invoke_result_t<std::decay_t<Function>&>
    {
        using Result = std::invoke_result_t<std::decay_t<Function>&>;
        auto signal = std::make_shared<EventSignal>();
        auto task = std::make_shared<std::packaged_task<Result()>>(
            std::forward<Function>(operation)
        );
        auto future = task->get_future();
        auto skipped = std::make_shared<std::atomic_bool>(false);
        const auto token = context ? context->token()
                                   : std::stop_token{};
        const auto deadline = context ? context->deadline()
                                      : std::nullopt;
        enqueue(
            [signal, task, skipped, token, deadline]
            {
                if (!token.stop_requested() &&
                    (!deadline ||
                     std::chrono::steady_clock::now() < *deadline))
                {
                    (*task)();
                }
                else
                {
                    skipped->store(true, std::memory_order_release);
                }
                signal->notify();
            }
        );
        const auto result = context ? io.waitReadable(
                                          signal->fd(),
                                          *context
                                      )
                                    : io.waitReadable(signal->fd());
        if (result != IOManager::WaitResult::Ready)
        {
            throw std::system_error(
                result == IOManager::WaitResult::TimedOut ? ETIMEDOUT
                                                          : ECANCELED,
                std::generic_category(),
                "blocking task wait"
            );
        }
        if (skipped->load(std::memory_order_acquire))
        {
            throw std::system_error(
                token.stop_requested() ? ECANCELED : ETIMEDOUT,
                std::generic_category(),
                "blocking task skipped"
            );
        }
        return future.get();
    }

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