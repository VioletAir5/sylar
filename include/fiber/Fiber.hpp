#pragma once

#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>

#include <boost/context/fiber.hpp>

namespace sylar
{

class Scheduler;

class Fiber : public std::enable_shared_from_this<Fiber>
{
public:
    using ptr = std::shared_ptr<Fiber>;

    enum State
    {
        INIT,
        HOLD,
        EXEC,
        TERM,
        READY,
        EXCEPT
    };

    explicit Fiber(
        std::function<void()> cb,
        std::size_t stacksize = 0,
        bool use_caller = false
    );
    ~Fiber();

    Fiber(const Fiber&) = delete;
    Fiber& operator=(const Fiber&) = delete;
    Fiber(Fiber&&) = delete;
    Fiber& operator=(Fiber&&) = delete;

    void reset(std::function<void()> cb);
    void swapIn();
    void swapOut();
    void call();
    void back();

    std::uint64_t getId() const noexcept;
    std::size_t getStackSize() const noexcept;
    State getState() const noexcept;
    void rethrowIfFailed() const;

    static void SetThis(Fiber* fiber) noexcept;
    static ptr GetThis();
    static Fiber* GetCurrent() noexcept;
    static void YieldToReady();
    static void YieldToHold();
    static std::uint64_t TotalFibers() noexcept;
    static std::uint64_t GetFiberId() noexcept;

private:
    Fiber(); // The current thread's main fiber has no separate stack.
    boost::context::fiber makeContext();
    void resume();
    void suspend();

    std::uint64_t m_id = 0;
    std::size_t m_stacksize = 0;
    State m_state = INIT;
    bool m_useCaller = false;
    Scheduler* m_scheduler = nullptr;
    std::function<void()> m_cb;
    std::exception_ptr m_error;
    boost::context::fiber m_caller;
    boost::context::fiber m_context;

    friend class Scheduler;
};

} // namespace sylar
