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

#include <grpcpp/grpcpp.h>
#include <grpcpp/support/async_unary_call.h>

#include "fiber/IOManager.hpp"
#include "fiber/RequestContext.hpp"

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
        )
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
        Prepare&& prepare
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

        auto call = std::make_shared<UnaryCall<Request, Reply>>(
            std::move(request)
        );
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
            return {std::move(admission), {}};
        }
        std::stop_callback cancel(
            request_context.token(),
            [call] { call->context.TryCancel(); }
        );
        IOManager::WaitResult outcome;
        try
        {
            outcome = io.waitReadable(
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
        return {call->status, std::move(call->reply)};
    }

    Snapshot snapshot() const;
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
        int fd;
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

    std::chrono::milliseconds m_maxTimeout;
    std::unique_ptr<Impl> m_impl;
};

int httpStatus(grpc::StatusCode code) noexcept;

} // namespace sylar::rpc
