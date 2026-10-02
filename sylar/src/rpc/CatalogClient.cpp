#include "rpc/CatalogClient.hpp"

#include <stdexcept>

namespace sylar::rpc
{

CatalogClient::CatalogClient(
    GrpcRuntime& runtime,
    const std::vector<std::string>& endpoints,
    std::shared_ptr<grpc::ChannelCredentials> credentials
)
    : m_runtime(runtime)
{
    if (endpoints.empty() || endpoints.size() > 128 || !credentials)
    {
        throw std::invalid_argument("invalid catalog endpoints");
    }
    for (const auto& endpoint : endpoints)
    {
        if (endpoint.empty() || endpoint.size() > 256)
        {
            throw std::invalid_argument("invalid catalog endpoint");
        }
        grpc::ChannelArguments arguments;
        arguments.SetMaxReceiveMessageSize(64 * 1024);
        arguments.SetMaxSendMessageSize(64 * 1024);
        arguments.SetInt("grpc.enable_retries", 0);
        m_stubs.push_back(
            v1::Catalog::NewStub(grpc::CreateCustomChannel(
                endpoint,
                credentials,
                arguments
            ))
        );
    }
}

UnaryResult<v1::ProductReply> CatalogClient::lookup(
    IOManager& io,
    const RequestContext& context,
    std::uint64_t product_id
)
{
    v1::ProductRequest request;
    request.set_product_id(product_id);
    const auto index = m_next.fetch_add(
                           1,
                           std::memory_order_relaxed
                       ) %
                       m_stubs.size();
    auto* stub = m_stubs[index].get();
    return m_runtime.unary<v1::ProductRequest, v1::ProductReply>(
        io,
        context,
        std::move(request),
        [stub](
            grpc::ClientContext* rpc_context,
            const v1::ProductRequest& value,
            grpc::CompletionQueue* queue
        ) {
            return stub
                ->PrepareAsyncGetProduct(rpc_context, value, queue);
        }
    );
}

} // namespace sylar::rpc
