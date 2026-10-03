#pragma once
#include <atomic>
#include <memory>
#include <string>
#include <vector>

#include "rpc/EndpointDirectory.hpp"
#include "rpc/GrpcRuntime.hpp"
namespace sylar::rpc
{
class CatalogClient
{
public:
    struct Options
    {
        std::string downstream = "catalog";
        std::size_t capacity = 0; // 0 uses runtime global limit for
                                  // backward compatibility.
        unsigned max_retries = 0;
        unsigned retry_budget = 4;
        std::chrono::milliseconds backoff{5};
        EndpointDirectory::Options endpoints;
    };
    struct Snapshot
    {
        std::uint64_t retries, retry_denied;
        unsigned retry_tokens;
    };
    CatalogClient(
        GrpcRuntime& runtime,
        const std::vector<std::string>& endpoints,
        std::shared_ptr<grpc::ChannelCredentials> credentials =
            grpc::InsecureChannelCredentials()
    );
    CatalogClient(
        GrpcRuntime& runtime,
        const std::vector<std::string>& endpoints,
        Options options,
        std::shared_ptr<grpc::ChannelCredentials> credentials =
            grpc::InsecureChannelCredentials()
    );
    UnaryResult<v1::ProductReply> lookup(
        IOManager& io,
        const RequestContext& context,
        std::uint64_t product_id
    );
    void updateEndpoints(const std::vector<std::string>& endpoints);
    std::vector<EndpointDirectory::Status> endpointStatus() const;
    Snapshot snapshot() const;

private:
    GrpcRuntime& m_runtime;
    Options m_options;
    EndpointDirectory m_directory;
    std::atomic_uint m_retryTokens;
    std::atomic_uint64_t m_retries{0}, m_retryDenied{0};
};
} // namespace sylar::rpc
