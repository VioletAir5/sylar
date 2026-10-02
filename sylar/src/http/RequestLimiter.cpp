#include "http/RequestLimiter.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace sylar
{

RequestLimiter::Permit::Permit(RequestLimiter* owner) noexcept
    : m_owner(owner)
    , m_started(std::chrono::steady_clock::now())
{
}

RequestLimiter::Permit::Permit(Permit&& other) noexcept
    : m_owner(std::exchange(other.m_owner, nullptr))
    , m_started(other.m_started)
{
}

RequestLimiter::Permit&
RequestLimiter::Permit::operator=(Permit&& other) noexcept
{
    if (this != &other)
    {
        release();
        m_owner = std::exchange(other.m_owner, nullptr);
        m_started = other.m_started;
    }
    return *this;
}

RequestLimiter::Permit::~Permit()
{
    release();
}

void RequestLimiter::Permit::release() noexcept
{
    if (auto* owner = std::exchange(m_owner, nullptr))
    {
        owner->complete(std::chrono::steady_clock::now() - m_started);
    }
}

RequestLimiter::RequestLimiter(Options options)
    : m_options(options)
    , m_limit(options.initial)
{
    if (options.minimum == 0 || options.minimum > options.initial ||
        options.initial > options.maximum || options.window < 4)
    {
        throw std::invalid_argument("invalid request limiter options"
        );
    }
}

std::optional<RequestLimiter::Permit>
RequestLimiter::tryAcquire() noexcept
{
    auto active = m_inFlight.load(std::memory_order_relaxed);
    for (;;)
    {
        if (active >= m_limit.load(std::memory_order_acquire))
        {
            m_rejected.fetch_add(1, std::memory_order_relaxed);
            m_windowRejected.fetch_add(1, std::memory_order_relaxed);
            return std::nullopt;
        }
        if (m_inFlight.compare_exchange_weak(
                active,
                active + 1,
                std::memory_order_acq_rel,
                std::memory_order_relaxed
            ))
        {
            break;
        }
    }
    auto peak = m_windowPeak.load(std::memory_order_relaxed);
    while (active + 1 > peak && !m_windowPeak.compare_exchange_weak(
                                    peak,
                                    active + 1,
                                    std::memory_order_relaxed
                                ))
    {
    }
    return Permit(this);
}

RequestLimiter::Snapshot RequestLimiter::snapshot() const noexcept
{
    return {
        .in_flight = m_inFlight.load(std::memory_order_relaxed),
        .limit = m_limit.load(std::memory_order_relaxed),
        .rejected = m_rejected.load(std::memory_order_relaxed),
        .completed = m_completed.load(std::memory_order_relaxed),
        .adaptive = m_options.adaptive,
    };
}

void RequestLimiter::complete(std::chrono::nanoseconds duration
) noexcept
{
    m_inFlight.fetch_sub(1, std::memory_order_acq_rel);
    m_completed.fetch_add(1, std::memory_order_relaxed);
    if (!m_options.adaptive)
    {
        return;
    }
    m_windowLatencyNs.fetch_add(
        static_cast<std::uint64_t>(duration.count()),
        std::memory_order_relaxed
    );
    if (m_windowCount.fetch_add(1, std::memory_order_relaxed) + 1 >=
        m_options.window)
    {
        updateWindow();
    }
}

void RequestLimiter::updateWindow() noexcept
{
    std::unique_lock lock(m_updateMutex, std::try_to_lock);
    if (!lock.owns_lock())
    {
        return;
    }
    const auto count = m_windowCount.exchange(
        0,
        std::memory_order_relaxed
    );
    if (count < m_options.window)
    {
        return;
    }
    const auto latency = m_windowLatencyNs.exchange(
        0,
        std::memory_order_relaxed
    );
    const auto peak = m_windowPeak.exchange(
        0,
        std::memory_order_relaxed
    );
    const auto rejected = m_windowRejected.exchange(
        0,
        std::memory_order_relaxed
    );
    const double average = static_cast<double>(latency) /
                           static_cast<double>(count);
    if (m_baselineNs == 0.0 || average < m_baselineNs)
    {
        m_baselineNs = average;
    }
    const auto current = m_limit.load(std::memory_order_relaxed);
    std::uint32_t next = current;
    if (average > m_baselineNs * 1.25 &&
        (peak >= current / 2 || rejected > 0))
    {
        next = std::max(
            m_options.minimum,
            static_cast<std::uint32_t>(
                static_cast<double>(current) * 0.8
            )
        );
    }
    else if (peak >= current || rejected > 0)
    {
        next = current + std::min(
                             m_options.maximum - current,
                             std::max(1U, current / 20)
                         );
    }
    m_limit.store(next, std::memory_order_release);
}

} // namespace sylar
