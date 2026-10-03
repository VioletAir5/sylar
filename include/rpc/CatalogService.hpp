#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>

#include "catalog.grpc.pb.h"
#include "http/RequestLimiter.hpp"
#include "rpc/Trace.hpp"

namespace sylar::rpc
{

// Read-only fixture catalog. Synchronous gRPC handlers use gRPC's own
// threads, never an IOManager thread. Delay is for fault
// demonstrations.
class CatalogService final : public v1::Catalog::Service
{
public:
    CatalogService(
        std::string instance_id,
        std::chrono::milliseconds delay = std::chrono::milliseconds(0
        ),
        std::uint32_t concurrency = 32,
        std::shared_ptr<TraceRecorder> recorder = {}
    );
    grpc::Status GetProduct(
        grpc::ServerContext* context,
        const v1::ProductRequest* request,
        v1::ProductReply* reply
    ) override;

    std::uint64_t cancelledCalls() const noexcept;

private:
    std::shared_ptr<TraceRecorder> m_recorder;
    std::string m_instanceId;
    std::chrono::milliseconds m_delay;
    RequestLimiter m_limiter;
    std::atomic_uint64_t m_cancelled{0};
};

} // namespace sylar::rpc
