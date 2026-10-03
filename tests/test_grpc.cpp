#include <atomic>
#include <chrono>
#include <future>
#include <thread>
#include <vector>

#include <gtest/gtest.h>
#include <yaml-cpp/yaml.h>

#include "fiber/IOManagerPool.hpp"
#include "rpc/CatalogClient.hpp"
#include "rpc/CatalogService.hpp"
using namespace std::chrono_literals;
namespace
{
class Backend
{
public:
    explicit Backend(
        std::chrono::milliseconds delay = 0ms, int wanted_port = 0
    )
        : service("test-backend", delay)
    {
        grpc::ServerBuilder builder;
        int port = 0;
        builder.AddListeningPort(
            "127.0.0.1:" + std::to_string(wanted_port),
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

TEST(GrpcTest, EndpointRemovalKeepsInFlightCallAlive)
{
    Backend slow(80ms), other;
    sylar::rpc::GrpcRuntime runtime;
    sylar::rpc::CatalogClient client(runtime, {slow.endpoint});
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
                                1
                            )
                            .status.ok());
        }
    );
    io.schedule(
        [&]
        {
            io.sleepFor(10ms);
            client.updateEndpoints({other.endpoint});
        }
    );
    io.run();
    EXPECT_EQ(client.endpointStatus().size(), 1);
    EXPECT_EQ(
        client.endpointStatus().front().address,
        other.endpoint
    );
    EXPECT_THROW(
        client.updateEndpoints({other.endpoint, other.endpoint}),
        std::invalid_argument
    );
    EXPECT_EQ(client.endpointStatus().size(), 1);
}
TEST(GrpcTest, IndependentDownstreamCapacity)
{
    Backend slow(100ms), fast;
    sylar::rpc::GrpcRuntime runtime(2);
    sylar::rpc::CatalogClient::Options a, b;
    a.downstream = "slow";
    a.capacity = 1;
    b.downstream = "fast";
    b.capacity = 1;
    sylar::rpc::CatalogClient first(runtime, {slow.endpoint}, a),
        second(runtime, {fast.endpoint}, b);
    sylar::IOManager io;
    io.schedule(
        [&]
        {
            EXPECT_TRUE(first
                            .lookup(
                                io,
                                sylar::RequestContext::withTimeout(1s
                                ),
                                1
                            )
                            .status.ok());
        }
    );
    io.schedule(
        [&]
        {
            EXPECT_EQ(
                first
                    .lookup(
                        io,
                        sylar::RequestContext::withTimeout(1s),
                        1
                    )
                    .status.error_code(),
                grpc::StatusCode::RESOURCE_EXHAUSTED
            );
            EXPECT_TRUE(second
                            .lookup(
                                io,
                                sylar::RequestContext::withTimeout(1s
                                ),
                                1
                            )
                            .status.ok());
        }
    );
    io.run();
    EXPECT_THROW(
        runtime.configureDownstream("overflow", 1),
        std::invalid_argument
    );
}
TEST(GrpcTest, RetryOnlyReadTransportFailuresWithinOriginalDeadline)
{
    Backend failed, healthy(20ms);
    failed.server->Shutdown();
    failed.server->Wait();
    sylar::rpc::GrpcRuntime runtime;
    sylar::rpc::CatalogClient::Options options;
    options.max_retries = 1;
    options.retry_budget = 1;
    sylar::rpc::CatalogClient client(
        runtime,
        {failed.endpoint, healthy.endpoint},
        options
    );
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
            EXPECT_EQ(client.snapshot().retries, 1);
            client.updateEndpoints({healthy.endpoint});
            EXPECT_EQ(
                client
                    .lookup(
                        io,
                        sylar::RequestContext::withTimeout(1s),
                        999
                    )
                    .status.error_code(),
                grpc::StatusCode::NOT_FOUND
            );
            EXPECT_EQ(client.snapshot().retries, 1);
        }
    );
    io.run();
    sylar::rpc::CatalogClient::Options bounded;
    bounded.max_retries = 1;
    bounded.backoff = 100ms;
    bounded.downstream = "bounded";
    sylar::rpc::CatalogClient short_client(
        runtime,
        {failed.endpoint, healthy.endpoint},
        bounded
    );
    io.schedule(
        [&]
        {
            auto start = std::chrono::steady_clock::now();
            EXPECT_EQ(
                short_client
                    .lookup(
                        io,
                        sylar::RequestContext::withTimeout(20ms),
                        1
                    )
                    .status.error_code(),
                grpc::StatusCode::DEADLINE_EXCEEDED
            );
            EXPECT_LT(
                std::chrono::steady_clock::now() - start,
                100ms
            );
            EXPECT_EQ(short_client.snapshot().retries, 0);
        }
    );
    io.run();
}
TEST(GrpcTest, RetryBudgetStopsFailureAmplification)
{
    Backend a, b;
    a.server->Shutdown();
    a.server->Wait();
    b.server->Shutdown();
    b.server->Wait();
    sylar::rpc::GrpcRuntime runtime;
    sylar::rpc::CatalogClient::Options options;
    options.max_retries = 1;
    options.retry_budget = 1;
    options.endpoints.failure_threshold = 100;
    sylar::rpc::CatalogClient client(
        runtime,
        {a.endpoint, b.endpoint},
        options
    );
    sylar::IOManager io;
    io.schedule(
        [&]
        {
            for (int n = 0; n < 3; ++n)
                EXPECT_FALSE(
                    client
                        .lookup(
                            io,
                            sylar::RequestContext::withTimeout(1s),
                            1
                        )
                        .status.ok()
                );
        }
    );
    io.run();
    EXPECT_EQ(client.snapshot().retries, 1);
    EXPECT_EQ(client.snapshot().retry_denied, 2);
    EXPECT_EQ(runtime.snapshot().started, 4);
}
TEST(GrpcTest, EjectAndProbeRecoverEndpoint)
{
    auto failed = std::make_unique<Backend>();
    Backend healthy;
    auto address = failed->endpoint;
    int port = std::stoi(address.substr(address.find(':') + 1));
    failed.reset();
    sylar::rpc::GrpcRuntime runtime;
    sylar::rpc::CatalogClient::Options options;
    options.endpoints.cooldown = 20ms;
    options.endpoints.probe_timeout = 50ms;
    sylar::rpc::CatalogClient client(
        runtime,
        {address, healthy.endpoint},
        options
    );
    sylar::IOManager io;
    io.schedule(
        [&]
        {
            for (int n = 0; n < 6; ++n)
                client.lookup(
                    io,
                    sylar::RequestContext::withTimeout(1s),
                    1
                );
            auto view = client.endpointStatus();
            EXPECT_TRUE(view.front().open);
            for (int n = 0; n < 4; ++n)
                EXPECT_TRUE(
                    client
                        .lookup(
                            io,
                            sylar::RequestContext::withTimeout(1s),
                            1
                        )
                        .status.ok()
                );
        }
    );
    io.run();
    Backend restarted(0ms, port);
    auto deadline = std::chrono::steady_clock::now() + 3s;
    while (client.endpointStatus().front().open &&
           std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(10ms);
    EXPECT_FALSE(client.endpointStatus().front().open);
}
TEST(GrpcTest, TracingPropagatesAndIsBounded)
{
    Backend backend;
    auto recorder = std::make_shared<sylar::rpc::TraceRecorder>(1);
    sylar::rpc::GrpcRuntime runtime(
        64,
        2s,
        sylar::rpc::GrpcRuntime::NotificationMode::WorkerQueue,
        recorder
    );
    sylar::rpc::CatalogClient client(runtime, {backend.endpoint});
    const auto parent = sylar::rpc::TraceContext::root();
    sylar::IOManager io;
    io.schedule(
        [&]
        {
            auto context = sylar::RequestContext::withTimeout(1s);
            context.setTraceParent(parent.header());
            auto value = client.lookup(io, context, 1);
            EXPECT_TRUE(value.status.ok());
            auto downstream = sylar::rpc::TraceContext::parse(
                value.reply.trace_parent()
            );
            ASSERT_TRUE(downstream);
            EXPECT_EQ(downstream->trace_id, parent.trace_id);
            EXPECT_NE(downstream->span_id, parent.span_id);
            client.lookup(io, context, 1);
        }
    );
    io.run();
    EXPECT_EQ(recorder->size(), 1);
    EXPECT_EQ(recorder->dropped(), 1);
    EXPECT_NE(
        recorder->json().find(parent.span_id),
        std::string::npos
    );
    EXPECT_THROW(
        sylar::rpc::TraceContext::parse(
            "00-00000000000000000000000000000000-0000000000000000-01"
        ),
        std::invalid_argument
    );
    auto unsampled = parent;
    unsampled.flags = "00";
    sylar::rpc::TraceScope
        span(recorder, unsampled.header(), "unsampled", "test");
    span.finish(0);
    EXPECT_EQ(recorder->dropped(), 1);
}

TEST(GrpcTest, ReservedCapacityCannotBeConsumedByGenericCalls)
{
    Backend backend;
    sylar::rpc::GrpcRuntime runtime(2);
    runtime.configureDownstream("protected-a", 1);
    runtime.configureDownstream("protected-b", 1);
    auto stub = sylar::rpc::v1::Catalog::NewStub(grpc::CreateChannel(
        backend.endpoint,
        grpc::InsecureChannelCredentials()
    ));
    sylar::IOManager io;
    io.schedule(
        [&]
        {
            sylar::rpc::v1::ProductRequest request;
            request.set_product_id(1);
            auto result = runtime.unary<
                sylar::rpc::v1::ProductRequest,
                sylar::rpc::v1::ProductReply>(
                io,
                sylar::RequestContext::withTimeout(1s),
                request,
                [&](auto* context, const auto& value, auto* queue) {
                    return stub->PrepareAsyncGetProduct(
                        context,
                        value,
                        queue
                    );
                }
            );
            EXPECT_EQ(
                result.status.error_code(),
                grpc::StatusCode::RESOURCE_EXHAUSTED
            );
        }
    );
    io.run();
    EXPECT_EQ(runtime.snapshot().started, 0);
}

} // namespace

TEST(GrpcTest, TraceSnapshotsSurviveConcurrentEviction)
{
    sylar::rpc::TraceRecorder recorder(8);
    std::atomic_bool go{false};
    std::jthread publisher(
        [&]
        {
            while (!go.load())
                std::this_thread::yield();
            for (unsigned i = 0; i < 1000; ++i)
            {
                sylar::rpc::SpanRecord span;
                span.trace_id = "0123456789abcdef0123456789abcdef";
                span.span_id = "0123456789abcdef";
                span.name = "generation-" + std::to_string(i);
                span.duration_us = i;
                recorder.record(std::move(span));
            }
        }
    );
    go.store(true);
    for (unsigned i = 0; i < 50; ++i)
    {
        const auto snapshot = YAML::Load(recorder.json());
        ASSERT_LE(snapshot["spans"].size(), 8);
        for (const auto& span : snapshot["spans"])
        {
            EXPECT_EQ(
                span["trace_id"].as<std::string>(),
                "0123456789abcdef0123456789abcdef"
            );
            EXPECT_EQ(
                span["name"].as<std::string>(),
                "generation-" +
                    std::to_string(span["duration_us"].as<unsigned>())
            );
        }
    }
    publisher.join();
    EXPECT_EQ(recorder.size(), 8);
    EXPECT_EQ(recorder.dropped(), 992);
}
