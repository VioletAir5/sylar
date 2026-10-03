#include "rpc/GrpcRuntime.hpp"

#include <sys/eventfd.h>
#include <unistd.h>

#include <cerrno>
#include <mutex>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <vector>

namespace sylar::rpc
{

GrpcRuntime::Call::Call() = default;
void GrpcRuntime::Call::prepareNotification(
    IOManager& io, NotificationMode mode
)
{
    if (mode == NotificationMode::WorkerQueue)
    {
        completion = io.makeCompletion();
        return;
    }
    fd = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (fd < 0)
        throw std::system_error(
            errno,
            std::generic_category(),
            "gRPC eventfd"
        );
}
GrpcRuntime::Call::~Call()
{
    if (fd >= 0)
        ::close(fd);
}
void GrpcRuntime::Call::notify() noexcept
{
    if (completion)
    {
        completion->notify();
        return;
    }
    const std::uint64_t one = 1;
    while (::write(fd, &one, sizeof(one)) < 0 && errno == EINTR)
    {
    }
}

struct GrpcRuntime::Impl
{
    explicit Impl(std::size_t maximum)
        : limit(maximum)
    {
    }

    grpc::CompletionQueue queue;
    mutable std::mutex mutex;
    std::once_flag shutdownOnce;
    std::unordered_map<Call*, std::shared_ptr<Call>> calls;
    struct Bulkhead
    {
        std::size_t limit = 0, in_flight = 0;
        std::uint64_t rejected = 0;
        bool reserved = false;
    };
    std::unordered_map<std::string, Bulkhead> bulkheads;
    std::thread poller;
    const std::size_t limit;
    bool stopping = false;
    std::uint64_t started = 0;
    std::uint64_t completed = 0;
    std::uint64_t rejected = 0;
    std::uint64_t failed = 0;
    std::uint64_t deadlines = 0;
    std::uint64_t cancelled = 0;
};

GrpcRuntime::GrpcRuntime(
    std::size_t maximum,
    std::chrono::milliseconds max_timeout,
    NotificationMode mode,
    std::shared_ptr<TraceRecorder> recorder
)
    : m_recorder(std::move(recorder))
    , m_mode(mode)
    , m_maxTimeout(max_timeout)
{
    if (maximum == 0 || maximum > 1000000 || max_timeout.count() <= 0)
    {
        throw std::invalid_argument("invalid gRPC runtime options");
    }
    m_impl = std::make_unique<Impl>(maximum);
    try
    {
        m_impl->poller = std::thread(
            [this]
            {
                void* raw = nullptr;
                bool ok = false;
                while (m_impl->queue.Next(&raw, &ok))
                {
                    std::unique_ptr<std::shared_ptr<Call>> tag(
                        static_cast<std::shared_ptr<Call>*>(raw)
                    );
                    const auto& call = *tag;
                    if (!ok && call->status.ok())
                    {
                        call->status = grpc::Status(
                            grpc::StatusCode::UNKNOWN,
                            "completion failed"
                        );
                    }
                    {
                        std::lock_guard lock(m_impl->mutex);
                        --m_impl->bulkheads.at(call->downstream)
                              .in_flight;
                        m_impl->calls.erase(call.get());
                        ++m_impl->completed;
                        if (!call->status.ok())
                        {
                            ++m_impl->failed;
                        }
                        if (call->status.error_code() ==
                            grpc::StatusCode::DEADLINE_EXCEEDED)
                        {
                            ++m_impl->deadlines;
                        }
                        if (call->status.error_code() ==
                            grpc::StatusCode::CANCELLED)
                        {
                            ++m_impl->cancelled;
                        }
                    }
                    call->completedAt = std::chrono::steady_clock::
                        now();
                    call->done.store(true, std::memory_order_release);
                    call->notify();
                }
            }
        );
    }
    catch (...)
    {
        m_impl->queue.Shutdown();
        void* tag = nullptr;
        bool ok = false;
        while (m_impl->queue.Next(&tag, &ok))
        {
        }
        throw;
    }
}

GrpcRuntime::~GrpcRuntime()
{
    shutdown();
}

grpc::Status GrpcRuntime::start(
    const std::shared_ptr<Call>& call,
    const std::function<void(grpc::CompletionQueue*, void*)>& initiate
)
{
    // Also serializes initiation against CQ Shutdown; no call may
    // start after the queue has begun shutting down.
    std::lock_guard lock(m_impl->mutex);
    if (m_impl->stopping)
    {
        return grpc::Status(
            grpc::StatusCode::UNAVAILABLE,
            "gRPC runtime stopping"
        );
    }

    if (!validRequestId(call->downstream) ||
        call->downstream.empty() ||
        (!m_impl->bulkheads.contains(call->downstream) &&
         m_impl->bulkheads.size() >= 64))
        return grpc::Status(
            grpc::StatusCode::INVALID_ARGUMENT,
            "invalid downstream"
        );
    auto& bulkhead = m_impl->bulkheads
                         .try_emplace(
                             call->downstream,
                             Impl::Bulkhead{m_impl->limit}
                         )
                         .first->second;
    std::size_t available = bulkhead.limit;
    if (!bulkhead.reserved)
    {
        available = m_impl->limit;
        for (const auto& [name, value] : m_impl->bulkheads)
            if (value.reserved)
                available -= value.limit;
    }
    if (bulkhead.in_flight >= available ||
        m_impl->calls.size() >= m_impl->limit)
    {
        ++m_impl->rejected;
        ++bulkhead.rejected;
        return grpc::Status(
            grpc::StatusCode::RESOURCE_EXHAUSTED,
            "gRPC in-flight limit"
        );
    }
    auto tag = std::make_unique<std::shared_ptr<Call>>(call);
    m_impl->calls.emplace(call.get(), call);
    try
    {
        initiate(&m_impl->queue, tag.get());
        tag.release();
        ++m_impl->started;
        ++bulkhead.in_flight;
    }
    catch (...)
    {
        m_impl->calls.erase(call.get());
        throw;
    }
    return grpc::Status::OK;
}

void GrpcRuntime::shutdown()
{
    std::call_once(
        m_impl->shutdownOnce,
        [this]
        {
            std::vector<std::shared_ptr<Call>> active;
            {
                std::lock_guard lock(m_impl->mutex);
                m_impl->stopping = true;
                for (const auto& [key, call] : m_impl->calls)
                {
                    (void)key;
                    active.push_back(call);
                }
            }
            for (const auto& call : active)
            {
                call->context.TryCancel();
            }
            m_impl->queue.Shutdown();
            m_impl->poller.join();
        }
    );
}

GrpcRuntime::Snapshot GrpcRuntime::snapshot() const
{
    std::lock_guard lock(m_impl->mutex);
    return {
        m_impl->calls.size(),
        m_impl->limit,
        m_impl->started,
        m_impl->completed,
        m_impl->rejected,
        m_impl->failed,
        m_impl->deadlines,
        m_impl->cancelled};
}

void GrpcRuntime::configureDownstream(
    const std::string& name, std::size_t capacity
)
{
    if (name.empty() || !validRequestId(name))
        throw std::invalid_argument("invalid downstream name");
    std::lock_guard lock(m_impl->mutex);
    bool reserved = capacity != 0;
    if (!capacity)
        capacity = m_impl->limit;
    if (capacity > m_impl->limit)
        throw std::invalid_argument(
            "downstream exceeds global capacity"
        );
    if (auto found = m_impl->bulkheads.find(name);
        found != m_impl->bulkheads.end())
    {
        if (found->second.limit != capacity ||
            found->second.reserved != reserved)
            throw std::invalid_argument(
                "inconsistent downstream capacity"
            );
        return;
    }
    if (!m_impl->calls.empty())
        throw std::logic_error("configure downstreams before traffic"
        );
    if (m_impl->bulkheads.size() >= 64)
        throw std::invalid_argument("too many downstreams");
    std::size_t total = reserved ? capacity : 0;
    for (auto& [key, value] : m_impl->bulkheads)
        if (value.reserved)
            total += value.limit;
    if (total > m_impl->limit)
        throw std::invalid_argument(
            "reserved capacities exceed global limit"
        );
    m_impl->bulkheads.emplace(
        name,
        Impl::Bulkhead{capacity, 0, 0, reserved}
    );
}
std::vector<GrpcRuntime::DownstreamSnapshot>
GrpcRuntime::downstreamSnapshot() const
{
    std::lock_guard lock(m_impl->mutex);
    std::vector<DownstreamSnapshot> result;
    for (auto& [name, value] : m_impl->bulkheads)
        result.push_back(
            {name, value.limit, value.in_flight, value.rejected}
        );
    return result;
}

bool GrpcRuntime::validRequestId(const std::string& id) noexcept
{
    if (id.size() > 128)
    {
        return false;
    }
    for (unsigned char ch : id)
    {
        if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
              (ch >= '0' && ch <= '9') || ch == '-' || ch == '_' ||
              ch == '.'))
        {
            return false;
        }
    }
    return true;
}

int httpStatus(grpc::StatusCode code) noexcept
{
    switch (code)
    {
    case grpc::StatusCode::OK:
        return 200;
    case grpc::StatusCode::INVALID_ARGUMENT:
        return 400;
    case grpc::StatusCode::NOT_FOUND:
        return 404;
    case grpc::StatusCode::DEADLINE_EXCEEDED:
        return 504;
    case grpc::StatusCode::RESOURCE_EXHAUSTED:
    case grpc::StatusCode::UNAVAILABLE:
    case grpc::StatusCode::CANCELLED:
        return 503;
    default:
        return 502;
    }
}

} // namespace sylar::rpc
