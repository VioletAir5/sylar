#include <atomic>
#include <chrono>
#include <future>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "fiber/IOManagerPool.hpp"
#include "rpc/CatalogClient.hpp"
#include "rpc/CatalogService.hpp"
using namespace std::chrono_literals;
namespace
{
class Backend
{
public:
    explicit Backend(std::chrono::milliseconds delay = 0ms)
        : service("test-backend", delay)
    {
        grpc::ServerBuilder builder;
        int port = 0;
        builder.AddListeningPort(
            "127.0.0.1:0",
            grpc::InsecureServerCredentials(),
            &port
        );
        builder.RegisterService(&service);
        server = builder.BuildAndStart();
        if (!server || port == 0)
            throw std::runtime_error("test RPC server failed");
        endpoint = "127.0.0.1:" + std::to_string(port);
    }
    ~Backend()
    {
        server->Shutdown(std::chrono::system_clock::now() + 1s);
        server->Wait();
    }
    sylar::rpc::CatalogService service;
    std::unique_ptr<grpc::Server> server;
    std::string endpoint;
};
TEST(GrpcTest, TypedReplyMetadataAndApplicationErrors)
{
    Backend backend;
    sylar::rpc::GrpcRuntime runtime;
    sylar::rpc::CatalogClient client(runtime, {backend.endpoint});
    sylar::IOManager io;
    io.schedule(
        [&]
        {
            auto context = sylar::RequestContext::withTimeout(
                1s,
                "interview-123"
            );
            auto value = client.lookup(io, context, 1);
            EXPECT_TRUE(value.status.ok())
                << value.status.error_message();
            EXPECT_EQ(value.reply.product_id(), 1);
            EXPECT_EQ(value.reply.price_cents(), 19900);
            EXPECT_EQ(value.reply.instance_id(), "test-backend");
            EXPECT_EQ(value.reply.request_id(), "interview-123");
            EXPECT_EQ(
                client.lookup(io, context, 4).status.error_code(),
                grpc::StatusCode::NOT_FOUND
            );
            EXPECT_EQ(
                client.lookup(io, context, 0).status.error_code(),
                grpc::StatusCode::INVALID_ARGUMENT
            );
        }
    );
    io.run();
    runtime.shutdown();
    EXPECT_EQ(runtime.snapshot().completed, 3);
    EXPECT_EQ(runtime.snapshot().in_flight, 0);
}
TEST(GrpcTest, ExpiredCancelledAndInvalidContextsNeverStart)
{
    Backend backend;
    sylar::rpc::GrpcRuntime runtime;
    sylar::rpc::CatalogClient client(runtime, {backend.endpoint});
    sylar::IOManager io;
    io.schedule(
        [&]
        {
            auto expired = sylar::RequestContext::withTimeout(0ms);
            EXPECT_EQ(
                client.lookup(io, expired, 1).status.error_code(),
                grpc::StatusCode::DEADLINE_EXCEEDED
            );
            sylar::RequestContext cancelled;
            cancelled.cancel();
            EXPECT_EQ(
                client.lookup(io, cancelled, 1).status.error_code(),
                grpc::StatusCode::CANCELLED
            );
            sylar::RequestContext invalid(
                std::nullopt,
                "bad\nmetadata"
            );
            EXPECT_EQ(
                client.lookup(io, invalid, 1).status.error_code(),
                grpc::StatusCode::INVALID_ARGUMENT
            );
        }
    );
    io.run();
    EXPECT_EQ(runtime.snapshot().started, 0);
}
TEST(GrpcTest, SlowRpcYieldsAndResumesOnOwnerThread)
{
    Backend backend(100ms);
    sylar::rpc::GrpcRuntime runtime;
    sylar::rpc::CatalogClient client(runtime, {backend.endpoint});
    sylar::IOManager io;
    bool peer_ran = false;
    const auto owner = std::this_thread::get_id();
    io.schedule(
        [&]
        {
            auto value = client.lookup(
                io,
                sylar::RequestContext::withTimeout(1s),
                1
            );
            EXPECT_TRUE(value.status.ok());
            EXPECT_TRUE(peer_ran);
            EXPECT_EQ(std::this_thread::get_id(), owner);
        }
    );
    io.schedule([&] { peer_ran = true; });
    io.run();
}
TEST(GrpcTest, DeadlineReleasesFiberAndDrainsCallState)
{
    Backend backend(500ms);
    sylar::rpc::GrpcRuntime runtime;
    sylar::rpc::CatalogClient client(runtime, {backend.endpoint});
    sylar::IOManager io;
    const auto start = std::chrono::steady_clock::now();
    io.schedule(
        [&]
        {
            auto value = client.lookup(
                io,
                sylar::RequestContext::withTimeout(30ms),
                1
            );
            EXPECT_EQ(
                value.status.error_code(),
                grpc::StatusCode::DEADLINE_EXCEEDED
            );
        }
    );
    io.run();
    EXPECT_LT(std::chrono::steady_clock::now() - start, 500ms);
    runtime.shutdown();
    EXPECT_EQ(runtime.snapshot().in_flight, 0);
    EXPECT_EQ(io.pendingEventCount(), 0);
}
TEST(GrpcTest, ExplicitCancellationIsObservedByBackend)
{
    Backend backend(500ms);
    sylar::rpc::GrpcRuntime runtime;
    sylar::rpc::CatalogClient client(runtime, {backend.endpoint});
    sylar::IOManager io;
    auto context = sylar::RequestContext::withTimeout(1s);
    io.schedule(
        [&]
        {
            EXPECT_EQ(
                client.lookup(io, context, 1).status.error_code(),
                grpc::StatusCode::CANCELLED
            );
        }
    );
    io.schedule(
        [&]
        {
            io.sleepFor(100ms);
            context.cancel();
        }
    );
    io.run();
    runtime.shutdown();
    const auto deadline = std::chrono::steady_clock::now() + 500ms;
    while (backend.service.cancelledCalls() == 0 &&
           std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(2ms);
    EXPECT_GT(backend.service.cancelledCalls(), 0);
    EXPECT_EQ(runtime.snapshot().in_flight, 0);
}
TEST(GrpcTest, CapacityRejectsWithoutQueuingAndRecovers)
{
    Backend backend(100ms);
    sylar::rpc::GrpcRuntime runtime(1);
    sylar::rpc::CatalogClient client(runtime, {backend.endpoint});
    sylar::IOManager io;
    io.schedule(
        [&]
        {
            EXPECT_TRUE(client
                            .lookup(
                                io,
                                sylar::RequestContext::withTimeout(1s
                                ),
                                1
                            )
                            .status.ok());
            EXPECT_TRUE(client
                            .lookup(
                                io,
                                sylar::RequestContext::withTimeout(1s
                                ),
                                2
                            )
                            .status.ok());
        }
    );
    io.schedule(
        [&]
        {
            EXPECT_EQ(
                client
                    .lookup(
                        io,
                        sylar::RequestContext::withTimeout(1s),
                        1
                    )
                    .status.error_code(),
                grpc::StatusCode::RESOURCE_EXHAUSTED
            );
        }
    );
    io.run();
    EXPECT_EQ(runtime.snapshot().rejected, 1);
    EXPECT_EQ(runtime.snapshot().started, 2);
    EXPECT_EQ(runtime.snapshot().in_flight, 0);
}
TEST(GrpcTest, ShutdownCancelsDrainsAndRejectsNewCalls)
{
    Backend backend(500ms);
    sylar::rpc::GrpcRuntime runtime;
    sylar::rpc::CatalogClient client(runtime, {backend.endpoint});
    sylar::IOManager io;
    io.schedule(
        [&]
        {
            auto value = client.lookup(
                io,
                sylar::RequestContext::withTimeout(1s),
                1
            );
            EXPECT_FALSE(value.status.ok());
            EXPECT_EQ(
                client
                    .lookup(
                        io,
                        sylar::RequestContext::withTimeout(1s),
                        1
                    )
                    .status.error_code(),
                grpc::StatusCode::UNAVAILABLE
            );
        }
    );
    io.schedule(
        [&]
        {
            io.sleepFor(20ms);
            runtime.shutdown();
        }
    );
    io.run();
    runtime.shutdown();
    EXPECT_EQ(runtime.snapshot().in_flight, 0);
    EXPECT_EQ(runtime.snapshot().completed, 1);
}
TEST(GrpcTest, PreparationExceptionDoesNotLeakAdmission)
{
    sylar::rpc::GrpcRuntime runtime(1);
    sylar::IOManager io;
    io.schedule(
        [&]
        {
            sylar::rpc::v1::ProductRequest request;
            EXPECT_THROW(
                (runtime.unary<
                    sylar::rpc::v1::ProductRequest,
                    sylar::rpc::v1::ProductReply>(
                    io,
                    sylar::RequestContext{},
                    request,
                    [](grpc::ClientContext*,
                       const auto&,
                       grpc::CompletionQueue*)
                        -> std::unique_ptr<
                            grpc::ClientAsyncResponseReader<
                                sylar::rpc::v1::ProductReply>>
                    { throw std::runtime_error("prepare failed"); }
                )),
                std::runtime_error
            );
        }
    );
    io.run();
    EXPECT_EQ(runtime.snapshot().in_flight, 0);
    EXPECT_EQ(runtime.snapshot().started, 0);
}
TEST(GrpcTest, SharedRuntimeCompletesAcrossFourWorkers)
{
    Backend backend(10ms);
    sylar::rpc::GrpcRuntime runtime(32);
    sylar::rpc::CatalogClient client(runtime, {backend.endpoint});
    sylar::IOManagerPool pool(4);
    std::vector<std::future<bool>> results;
    for (int i = 0; i < 16; ++i)
    {
        auto promise = std::make_shared<std::promise<bool>>();
        results.push_back(promise->get_future());
        pool.post(
            [&, promise](sylar::IOManager& io)
            {
                try
                {
                    const auto owner = std::this_thread::get_id();
                    auto value = client.lookup(
                        io,
                        sylar::RequestContext::withTimeout(1s),
                        1
                    );
                    promise->set_value(
                        value.status.ok() &&
                        owner == std::this_thread::get_id()
                    );
                }
                catch (...)
                {
                    promise->set_value(false);
                }
            }
        );
    }
    for (auto& result : results)
    {
        const auto ready = result.wait_for(3s);
        EXPECT_EQ(ready, std::future_status::ready);
        if (ready == std::future_status::ready)
            EXPECT_TRUE(result.get());
    }
    pool.stop();
    runtime.shutdown();
    EXPECT_EQ(runtime.snapshot().completed, 16);
    EXPECT_EQ(runtime.snapshot().in_flight, 0);
}
TEST(GrpcTest, CallStateOutlivesStoppedIOManager)
{
    Backend backend(500ms);
    sylar::rpc::GrpcRuntime runtime;
    sylar::rpc::CatalogClient client(runtime, {backend.endpoint});
    {
        sylar::IOManager io;
        io.schedule(
            [&]
            {
                auto value = client.lookup(
                    io,
                    sylar::RequestContext::withTimeout(1s),
                    1
                );
                EXPECT_EQ(
                    value.status.error_code(),
                    grpc::StatusCode::CANCELLED
                );
            }
        );
        io.schedule(
            [&]
            {
                io.sleepFor(10ms);
                io.requestStop();
            }
        );
        io.runForever();
    }
    // CQ cancellation may finish after the owner IOManager is
    // destroyed.
    runtime.shutdown();
    EXPECT_EQ(runtime.snapshot().in_flight, 0);
    EXPECT_EQ(runtime.snapshot().completed, 1);
}

} // namespace
