#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <optional>

namespace sylar
{

// Bounds concurrent HTTP requests after parsing. Adaptive mode is
// experimental and can be disabled without changing connection
// limits.
class RequestLimiter
{
public:
    struct Options
    {
        std::uint32_t initial = 1024;
        std::uint32_t minimum = 16;
        std::uint32_t maximum = 1024;
        std::uint32_t window = 64;
        bool adaptive = false;
    };

    struct Snapshot
    {
        std::uint32_t in_flight = 0;
        std::uint32_t limit = 0;
        std::uint64_t rejected = 0;
        std::uint64_t completed = 0;
        bool adaptive = false;
    };

    class Permit
    {
    public:
        Permit(const Permit&) = delete;
        Permit& operator=(const Permit&) = delete;
        Permit(Permit&& other) noexcept;
        Permit& operator=(Permit&& other) noexcept;
        ~Permit();

    private:
        friend class RequestLimiter;
        explicit Permit(RequestLimiter* owner) noexcept;
        void release() noexcept;

        RequestLimiter* m_owner = nullptr;
        std::chrono::steady_clock::time_point m_started;
    };

    explicit RequestLimiter(Options options);
    std::optional<Permit> tryAcquire() noexcept;
    Snapshot snapshot() const noexcept;

private:
    void complete(std::chrono::nanoseconds duration) noexcept;
    void updateWindow() noexcept;

    Options m_options;
    std::atomic_uint32_t m_limit;
    std::atomic_uint32_t m_inFlight{0};
    std::atomic_uint32_t m_windowPeak{0};
    std::atomic_uint64_t m_rejected{0};
    std::atomic_uint64_t m_completed{0};
    std::atomic_uint64_t m_windowRejected{0};
    std::atomic_uint64_t m_windowCount{0};
    std::atomic_uint64_t m_windowLatencyNs{0};
    std::mutex m_updateMutex;
    double m_baselineNs = 0.0;
};

} // namespace sylar
