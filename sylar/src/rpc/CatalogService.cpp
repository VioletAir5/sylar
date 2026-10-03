#include "rpc/CatalogService.hpp"

#include <stdexcept>
#include <thread>

#include "rpc/GrpcRuntime.hpp"

namespace sylar::rpc
{

CatalogService::CatalogService(
    std::string instance_id,
    std::chrono::milliseconds delay,
    std::uint32_t concurrency,
    std::shared_ptr<TraceRecorder> recorder
)
    : m_recorder(std::move(recorder))
    , m_instanceId(std::move(instance_id))
    , m_delay(delay)
    , m_limiter(RequestLimiter::Options{
          .initial = concurrency,
          .minimum = concurrency,
          .maximum = concurrency})
{
    if (!GrpcRuntime::validRequestId(m_instanceId) ||
        m_instanceId.empty() || delay.count() < 0 ||
        delay.count() > 5000)
    {
        throw std::invalid_argument("invalid catalog service options"
        );
    }
}

grpc::Status CatalogService::GetProduct(
    grpc::ServerContext* context,
    const v1::ProductRequest* request,
    v1::ProductReply* reply
)
{

    const auto started = std::chrono::steady_clock::now();
    std::string parent;
    if (auto found = context->client_metadata().find("traceparent");
        found != context->client_metadata().end())
        parent.assign(found->second.data(), found->second.size());
    try
    {
        TraceContext::parse(parent);
    }
    catch (...)
    {
        return grpc::Status(
            grpc::StatusCode::INVALID_ARGUMENT,
            "invalid traceparent"
        );
    }
    std::optional<TraceScope> span;
    if (m_recorder || !parent.empty())
        span.emplace(
            m_recorder,
            parent,
            "Catalog.GetProduct",
            "rpc.server"
        );
    auto execute = [&]() -> grpc::Status
    {
        auto permit = m_limiter.tryAcquire();
        if (!permit)
        {
            return grpc::Status(
                grpc::StatusCode::RESOURCE_EXHAUSTED,
                "catalog busy"
            );
        }
        const auto until = std::chrono::steady_clock::now() + m_delay;
        while (std::chrono::steady_clock::now() < until)
        {
            if (context->IsCancelled())
            {
                m_cancelled.fetch_add(1, std::memory_order_relaxed);
                return grpc::Status(
                    grpc::StatusCode::CANCELLED,
                    "catalog cancelled"
                );
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        if (context->IsCancelled())
        {
            m_cancelled.fetch_add(1, std::memory_order_relaxed);
            return grpc::Status(
                grpc::StatusCode::CANCELLED,
                "catalog cancelled"
            );
        }
        if (request->product_id() == 0)
        {
            return grpc::Status(
                grpc::StatusCode::INVALID_ARGUMENT,
                "zero product ID"
            );
        }
        if (request->product_id() > 3)
        {
            return grpc::Status(
                grpc::StatusCode::NOT_FOUND,
                "product not found"
            );
        }
        static constexpr const char* names[] = {
            "keyboard",
            "mouse",
            "monitor"};
        static constexpr std::uint64_t prices[] = {
            19900,
            9900,
            129900};
        const auto index = request->product_id() - 1;
        reply->set_product_id(request->product_id());
        reply->set_name(names[index]);
        reply->set_price_cents(prices[index]);
        reply->set_instance_id(m_instanceId);
        const auto found = context->client_metadata().find(
            "x-request-id"
        );
        if (found != context->client_metadata().end())
        {
            reply->set_request_id(std::string(
                found->second.data(),
                found->second.size()
            ));
        }
        return grpc::Status::OK;
    };
    auto status = execute();
    if (span)
        span->finish(static_cast<int>(status.error_code()));
    if (status.ok())
    {
        reply->set_trace_parent(span ? span->header() : "");
        reply->set_server_elapsed_us(
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - started
            )
                .count()
        );
    }
    return status;
}

std::uint64_t CatalogService::cancelledCalls() const noexcept
{
    return m_cancelled.load(std::memory_order_relaxed);
}

} // namespace sylar::rpc
