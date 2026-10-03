#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>

#include <grpcpp/grpcpp.h>
#include <grpcpp/support/async_unary_call.h>

#include "fiber/IOManager.hpp"
#include "fiber/RequestContext.hpp"
#include "rpc/Trace.hpp"

namespace sylar::rpc
{

template <class Reply> struct UnaryResult
{
    grpc::Status status;
    Reply reply;
};

// One CQ thread serves many outstanding unary calls. eventfd
// completion notification resumes the calling Fiber on its own
// IOManager.
class GrpcRuntime
{
public:
    enum class NotificationMode
    {
        WorkerQueue,
        PerCallEventfd
    };
    struct Snapshot
    {
        std::size_t in_flight;
        std::size_t limit;
        std::uint64_t started;
        std::uint64_t completed;
        std::uint64_t rejected;
        std::uint64_t failed;
        std::uint64_t deadlines;
        std::uint64_t cancelled;
    };

    explicit GrpcRuntime(
        std::size_t max_in_flight = 64,
        std::chrono::milliseconds max_timeout = std::chrono::seconds(2
        ),
        NotificationMode mode = NotificationMode::WorkerQueue,
        std::shared_ptr<TraceRecorder> recorder = {}
    );
    ~GrpcRuntime();
    GrpcRuntime(const GrpcRuntime&) = delete;
    GrpcRuntime& operator=(const GrpcRuntime&) = delete;

    // Prepare must return a generated PrepareAsync unary reader.
    // Requests and replies stay owned until CQ completion, even if
    // the caller's deadline/cancellation releases the Fiber earlier.
    template <class Request, class Reply, class Prepare>
    UnaryResult<Reply> unary(
        IOManager& io,
        const RequestContext& request_context,
        Request request,
        Prepare&& prepare,
        const std::string& downstream = "default"
    )
    {
        if (IOManager::GetThis() != &io)
        {
            throw std::logic_error("gRPC requires an IOManager Fiber"
            );
        }
        if (request_context.cancelled())
        {
            return {
                grpc::Status(
                    grpc::StatusCode::CANCELLED,
                    "request cancelled"
                ),
                {}};
        }
        auto timeout = m_maxTimeout;
        if (auto remaining = request_context.remaining())
        {
            timeout = std::min(timeout, *remaining);
        }
        if (timeout.count() <= 0)
        {
            return {
                grpc::Status(
                    grpc::StatusCode::DEADLINE_EXCEEDED,
                    "request expired"
                ),
                {}};
        }
        if (!validRequestId(request_context.requestId()))
        {
            return {
                grpc::Status(
                    grpc::StatusCode::INVALID_ARGUMENT,
                    "invalid request id"
                ),
                {}};
        }

        std::optional<TraceScope> span;
        if (m_recorder || !request_context.traceParent().empty())
            span.emplace(
                m_recorder,
                request_context.traceParent(),
                downstream,
                "rpc.client"
            );
        auto call = std::make_shared<UnaryCall<Request, Reply>>(
            std::move(request)
        );
        call->downstream = downstream;
        call->prepareNotification(io, m_mode);
        call->context.set_deadline(
            std::chrono::system_clock::now() + timeout
        );
        if (!request_context.requestId().empty())
        {
            call->context.AddMetadata(
                "x-request-id",
                request_context.requestId()
            );
        }
        if (span)
            call->context.AddMetadata("traceparent", span->header());
        grpc::Status admission = start(
            call,
            [&](grpc::CompletionQueue* queue, void* tag)
            {
                call->reader = prepare(
                    &call->context,
                    call->request,
                    queue
                );
                if (!call->reader)
                {
                    throw std::runtime_error("empty gRPC unary reader"
                    );
                }
                call->reader->StartCall();
                call->reader
                    ->Finish(&call->reply, &call->status, tag);
            }
        );
        if (!admission.ok())
        {
            if (span)
                span->finish(static_cast<int>(admission.error_code())
                );
            return {std::move(admission), {}};
        }
        std::stop_callback cancel(
            request_context.token(),
            [call] { call->context.TryCancel(); }
        );
        IOManager::WaitResult outcome;
        try
        {
            outcome = call->completion ? io.waitCompletion(
                                             *call->completion,
                                             timeout,
                                             request_context
                                         )
                                       : io.waitReadable(
                                             call->fd,
                                             timeout,
                                             request_context
                                         );
        }
        catch (...)
        {
            call->context.TryCancel();
            throw;
        }
        if (outcome != IOManager::WaitResult::Ready)
        {
            call->context.TryCancel();
            if (span)
                span->finish(static_cast<int>(
                    outcome == IOManager::WaitResult::TimedOut
                        ? grpc::StatusCode::DEADLINE_EXCEEDED
                        : grpc::StatusCode::CANCELLED
                ));
            return {
                grpc::Status(
                    outcome == IOManager::WaitResult::TimedOut
                        ? grpc::StatusCode::DEADLINE_EXCEEDED
                        : grpc::StatusCode::CANCELLED,
                    "gRPC Fiber wait ended"
                ),
                {}};
        }
        if (!call->done.load(std::memory_order_acquire))
        {
            call->context.TryCancel();
            throw std::logic_error("gRPC signalled before completion"
            );
        }
        if (span)
            span->finish(
                static_cast<int>(call->status.error_code()),
                std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now() -
                    call->completedAt
                )
                    .count()
            );
        return {call->status, std::move(call->reply)};
    }

    Snapshot snapshot() const;
    struct DownstreamSnapshot
    {
        std::string name;
        std::size_t limit, in_flight;
        std::uint64_t rejected;
    };
    void configureDownstream(
        const std::string& name, std::size_t capacity
    );
    std::vector<DownstreamSnapshot> downstreamSnapshot() const;
    // Reject new calls, cancel active RPCs, drain CQ tags, then join.
    // Callers must stop using this object before destruction.
    void shutdown();
    static bool validRequestId(const std::string& id) noexcept;

private:
    struct Call
    {
        Call();
        virtual ~Call();
        grpc::ClientContext context;
        grpc::Status status;
        std::atomic_bool done{false};
        std::chrono::steady_clock::time_point completedAt;
        std::string downstream;
        int fd = -1;
        std::optional<IOManager::Completion> completion;
        void
        prepareNotification(IOManager& io, NotificationMode mode);
        void notify() noexcept;
    };

    template <class Request, class Reply>
    struct UnaryCall final : Call
    {
        explicit UnaryCall(Request value)
            : request(std::move(value))
        {
        }
        Request request;
        Reply reply;
        std::unique_ptr<grpc::ClientAsyncResponseReader<Reply>>
            reader;
    };

    struct Impl;
    grpc::Status start(
        const std::shared_ptr<Call>& call,
        const std::function<void(grpc::CompletionQueue*, void*)>&
            initiate
    );

    std::shared_ptr<TraceRecorder> m_recorder;
    NotificationMode m_mode;
    std::chrono::milliseconds m_maxTimeout;
    std::unique_ptr<Impl> m_impl;
};

int httpStatus(grpc::StatusCode code) noexcept;

} // namespace sylar::rpc
