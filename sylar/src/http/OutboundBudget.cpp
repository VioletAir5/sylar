#include "http/OutboundBudget.hpp"

#include <stdexcept>
#include <utility>

namespace sylar
{

OutboundBudget::Lease::Lease(
    std::shared_ptr<OutboundBudget> owner, std::size_t bytes
) noexcept
    : m_owner(std::move(owner))
    , m_bytes(bytes)
{
}

OutboundBudget::Lease::Lease(Lease&& other) noexcept
    : m_owner(std::move(other.m_owner))
    , m_bytes(std::exchange(other.m_bytes, 0))
{
}

OutboundBudget::Lease& OutboundBudget::Lease::operator=(Lease&& other
) noexcept
{
    if (this != &other)
    {
        release();
        m_owner = std::move(other.m_owner);
        m_bytes = std::exchange(other.m_bytes, 0);
    }
    return *this;
}

OutboundBudget::Lease::~Lease()
{
    release();
}

void OutboundBudget::Lease::release() noexcept
{
    if (m_owner)
    {
        m_owner->m_inFlight.fetch_sub(
            m_bytes,
            std::memory_order_acq_rel
        );
        m_owner.reset();
        m_bytes = 0;
    }
}

std::shared_ptr<OutboundBudget> OutboundBudget::processDefault()
{
    static auto budget = std::make_shared<OutboundBudget>(
        64 * 1024 * 1024
    );
    return budget;
}

OutboundBudget::OutboundBudget(std::size_t maximum)
    : m_maximum(maximum)
{
    if (maximum == 0)
    {
        throw std::invalid_argument("zero outbound budget");
    }
}

std::optional<OutboundBudget::Lease>
OutboundBudget::tryReserve(std::size_t bytes)
{
    auto current = m_inFlight.load(std::memory_order_relaxed);
    do
    {
        if (bytes > m_maximum - current)
        {
            m_rejected.fetch_add(1, std::memory_order_relaxed);
            return std::nullopt;
        }
    } while (!m_inFlight.compare_exchange_weak(
        current,
        current + bytes,
        std::memory_order_acq_rel,
        std::memory_order_relaxed
    ));
    return Lease(shared_from_this(), bytes);
}

std::size_t OutboundBudget::inFlightBytes() const noexcept
{
    return m_inFlight.load(std::memory_order_relaxed);
}

std::size_t OutboundBudget::maximumBytes() const noexcept
{
    return m_maximum;
}

std::size_t OutboundBudget::rejected() const noexcept
{
    return m_rejected.load(std::memory_order_relaxed);
}

} // namespace sylar
