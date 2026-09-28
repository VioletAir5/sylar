#pragma once

#include <chrono>

namespace sylar
{

// Opt-in POSIX I/O interception for the current thread. When an
// IOManager Fiber calls read/write/recv/send/connect/accept on a
// nonblocking socket, EAGAIN suspends the Fiber instead of the
// thread. Other descriptors retain their normal POSIX behavior.
class HookScope
{
public:
    explicit HookScope(
        std::chrono::milliseconds timeout = std::chrono::seconds(30)
    );
    ~HookScope();

    HookScope(const HookScope&) = delete;
    HookScope& operator=(const HookScope&) = delete;

    static bool enabled() noexcept;

private:
    std::chrono::milliseconds m_timeout;
};

} // namespace sylar
