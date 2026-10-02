#pragma once

#include <chrono>
#include <optional>
#include <stop_token>
#include <string>
#include <utility>

namespace sylar
{

// Shared by a request's I/O and optional blocking work. Cancellation
// is cooperative; it never terminates a running third-party function.
class RequestContext
{
public:
    explicit RequestContext(
        std::optional<std::chrono::steady_clock::time_point>
            deadline = std::nullopt,
        std::string request_id = {}
    )
        : m_deadline(deadline)
        , m_requestId(std::move(request_id))
    {
    }

    static RequestContext withTimeout(
        std::chrono::milliseconds timeout, std::string request_id = {}
    )
    {
        return RequestContext(
            std::chrono::steady_clock::now() + timeout,
            std::move(request_id)
        );
    }

    std::optional<std::chrono::steady_clock::time_point>
    deadline() const noexcept
    {
        return m_deadline;
    }

    std::optional<std::chrono::milliseconds> remaining() const
    {
        if (!m_deadline)
        {
            return std::nullopt;
        }
        const auto now = std::chrono::steady_clock::now();
        if (now >= *m_deadline)
        {
            return std::chrono::milliseconds::zero();
        }
        return std::chrono::ceil<std::chrono::milliseconds>(
            *m_deadline - now
        );
    }

    bool expired() const
    {
        return m_deadline &&
               std::chrono::steady_clock::now() >= *m_deadline;
    }

    std::stop_token token() const noexcept
    {
        return m_stop.get_token();
    }

    bool cancel() noexcept
    {
        return m_stop.request_stop();
    }

    bool cancelled() const noexcept
    {
        return m_stop.stop_requested();
    }

    const std::string& requestId() const noexcept
    {
        return m_requestId;
    }

private:
    std::optional<std::chrono::steady_clock::time_point> m_deadline;
    std::stop_source m_stop;
    std::string m_requestId;
};

} // namespace sylar
