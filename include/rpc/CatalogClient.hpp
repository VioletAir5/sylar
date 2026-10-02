#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "catalog.grpc.pb.h"
#include "rpc/GrpcRuntime.hpp"

namespace sylar::rpc
{

// Static endpoint routing for the demo. Shared channels are reused;
// no automatic application retries or discovery are performed.
class CatalogClient
{
public:
    CatalogClient(
        GrpcRuntime& runtime,
        const std::vector<std::string>& endpoints,
        std::shared_ptr<grpc::ChannelCredentials> credentials =
            grpc::InsecureChannelCredentials()
    );

    UnaryResult<v1::ProductReply> lookup(
        IOManager& io,
        const RequestContext& context,
        std::uint64_t product_id
    );

private:
    GrpcRuntime& m_runtime;
    std::vector<std::unique_ptr<v1::Catalog::Stub>> m_stubs;
    std::atomic_size_t m_next{0};
};

} // namespace sylar::rpc
