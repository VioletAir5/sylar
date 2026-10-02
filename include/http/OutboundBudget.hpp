#pragma once

#include <atomic>
#include <cstddef>
#include <memory>
#include <optional>

namespace sylar
{

// Bounds bytes held by concurrent WebSocket frame writers.
class OutboundBudget
    : public std::enable_shared_from_this<OutboundBudget>
{
public:
    class Lease
    {
    public:
        Lease(const Lease&) = delete;
        Lease& operator=(const Lease&) = delete;
        Lease(Lease&& other) noexcept;
        Lease& operator=(Lease&& other) noexcept;
        ~Lease();

    private:
        friend class OutboundBudget;
        Lease(
            std::shared_ptr<OutboundBudget> owner, std::size_t bytes
        ) noexcept;
        void release() noexcept;
        std::shared_ptr<OutboundBudget> m_owner;
        std::size_t m_bytes = 0;
    };

    explicit OutboundBudget(std::size_t maximum);
    static std::shared_ptr<OutboundBudget> processDefault();
    std::optional<Lease> tryReserve(std::size_t bytes);
    std::size_t inFlightBytes() const noexcept;
    std::size_t maximumBytes() const noexcept;
    std::size_t rejected() const noexcept;

private:
    std::size_t m_maximum;
    std::atomic_size_t m_inFlight{0};
    std::atomic_size_t m_rejected{0};
};

} // namespace sylar
