#include "rpc/CatalogClient.hpp"

#include <stdexcept>
namespace sylar::rpc
{
CatalogClient::CatalogClient(
    GrpcRuntime& runtime,
    const std::vector<std::string>& endpoints,
    std::shared_ptr<grpc::ChannelCredentials> credentials
)
    : CatalogClient(
          runtime, endpoints, Options{}, std::move(credentials)
      )
{
}
CatalogClient::CatalogClient(
    GrpcRuntime& runtime,
    const std::vector<std::string>& endpoints,
    Options options,
    std::shared_ptr<grpc::ChannelCredentials> credentials
)
    : m_runtime(runtime)
    , m_options(std::move(options))
    , m_directory(
          endpoints, m_options.endpoints, std::move(credentials)
      )
    , m_retryTokens(m_options.retry_budget)
{
    if (m_options.max_retries > 2 || m_options.backoff.count() < 0 ||
        m_options.backoff > std::chrono::seconds(1) ||
        m_options.retry_budget > 1000)
        throw std::invalid_argument("invalid retry options");
    m_runtime.configureDownstream(
        m_options.downstream,
        m_options.capacity
    );
}
void CatalogClient::updateEndpoints(
    const std::vector<std::string>& endpoints
)
{
    m_directory.update(endpoints);
}
std::vector<EndpointDirectory::Status>
CatalogClient::endpointStatus() const
{
    return m_directory.snapshot();
}
CatalogClient::Snapshot CatalogClient::snapshot() const
{
    return {
        m_retries.load(),
        m_retryDenied.load(),
        m_retryTokens.load()};
}
UnaryResult<v1::ProductReply> CatalogClient::lookup(
    IOManager& io,
    const RequestContext& context,
    std::uint64_t product_id
)
{
    if (!context.deadline())
    {
        auto bounded = RequestContext::withTimeout(
            std::chrono::seconds(2),
            context.requestId()
        );
        bounded.setTraceParent(context.traceParent());
        std::stop_callback cancel(
            context.token(),
            [&] { bounded.cancel(); }
        );
        return lookup(io, bounded, product_id);
    }
    UnaryResult<v1::ProductReply> result{
        grpc::Status(
            grpc::StatusCode::UNAVAILABLE,
            "no healthy endpoint"
        ),
        {}};
    std::string previous;
    for (unsigned attempt = 0; attempt <= m_options.max_retries;
         ++attempt)
    {
        if (context.cancelled())
            return {
                grpc::Status(
                    grpc::StatusCode::CANCELLED,
                    "cancelled"
                ),
                {}};
        if (context.expired())
            return {
                grpc::Status(
                    grpc::StatusCode::DEADLINE_EXCEEDED,
                    "expired"
                ),
                {}};
        auto endpoint = m_directory.select(previous);
        if (!endpoint)
            return result;
        previous = endpoint->address;
        if (attempt > 0)
        {
            auto tokens = m_retryTokens.load();
            while (tokens && !m_retryTokens.compare_exchange_weak(
                                 tokens,
                                 tokens - 1
                             ))
            {
            }
            if (!tokens)
            {
                ++m_retryDenied;
                return result;
            }
            // Interruptible backoff shares the original request
            // budget.
            auto completion = io.makeCompletion();
            auto wait = io.waitCompletion(
                completion,
                m_options.backoff +
                    std::chrono::milliseconds(attempt),
                context
            );
            if (wait == IOManager::WaitResult::Cancelled)
                return {
                    grpc::Status(
                        grpc::StatusCode::CANCELLED,
                        "retry cancelled"
                    ),
                    {}};
            if (context.expired())
                return {
                    grpc::Status(
                        grpc::StatusCode::DEADLINE_EXCEEDED,
                        "retry budget expired"
                    ),
                    {}};
            ++m_retries;
        }
        v1::ProductRequest request;
        request.set_product_id(product_id);
        result = m_runtime
                     .unary<v1::ProductRequest, v1::ProductReply>(
                         io,
                         context,
                         std::move(request),
                         [endpoint](
                             grpc::ClientContext* rpc_context,
                             const auto& value,
                             grpc::CompletionQueue* queue
                         )
                         {
                             return endpoint->stub
                                 ->PrepareAsyncGetProduct(
                                     rpc_context,
                                     value,
                                     queue
                                 );
                         },
                         m_options.downstream
                     );
        m_directory.observe(endpoint, result.status.error_code());
        if (result.status.ok())
        {
            if (attempt == 0)
            {
                auto tokens = m_retryTokens.load();
                while (tokens < m_options.retry_budget &&
                       !m_retryTokens.compare_exchange_weak(
                           tokens,
                           tokens + 1
                       ))
                {
                }
            }
            return result;
        }
        // Only this explicitly read-only operation retries
        // unavailable transport.
        if (result.status.error_code() !=
            grpc::StatusCode::UNAVAILABLE)
            return result;
    }
    return result;
}
} // namespace sylar::rpc
