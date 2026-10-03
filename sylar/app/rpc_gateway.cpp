#include <sys/signalfd.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

#include <fmt/format.h>

#include "os/SignalFd.hpp"

#include "fiber/IOManagerPool.hpp"
#include "http/HttpServer.hpp"
#include "rpc/CatalogClient.hpp"
#ifdef SYLAR_RPC_WITH_ZOOKEEPER
#include "rpc/ZkDiscovery.hpp"
#endif
namespace
{
unsigned
number(std::string_view text, unsigned maximum, bool zero = false)
{
    unsigned value = 0;
    auto [end, error] = std::from_chars(
        text.data(),
        text.data() + text.size(),
        value
    );
    if (error != std::errc{} || end != text.data() + text.size() ||
        value > maximum || (!zero && !value))
        throw std::invalid_argument("invalid numeric argument");
    return value;
}
unsigned envNumber(
    const char* name,
    unsigned fallback,
    unsigned maximum,
    bool zero = false
)
{
    auto* value = std::getenv(name);
    return value && *value ? number(value, maximum, zero) : fallback;
}
std::vector<std::string> endpoints(std::string_view text)
{
    std::vector<std::string> result;
    for (;;)
    {
        auto comma = text.find(',');
        auto part = text.substr(0, comma);
        if (part.empty())
            throw std::invalid_argument("empty endpoint");
        result.emplace_back(part);
        if (comma == std::string_view::npos)
            break;
        text.remove_prefix(comma + 1);
    }
    return result;
}
std::string jsonString(std::string_view text)
{
    std::string result = "\"";
    for (unsigned char c : text)
    {
        if (c == '"' || c == '\\')
        {
            result += '\\';
            result += c;
        }
        else if (c < 32)
            result += fmt::format("\\u{:04x}", c);
        else
            result += c;
    }
    return result + '"';
}
std::string reason(int status)
{
    switch (status)
    {
    case 200:
        return "OK";
    case 400:
        return "Bad Request";
    case 404:
        return "Not Found";
    case 503:
        return "Service Unavailable";
    case 504:
        return "Gateway Timeout";
    default:
        return "Bad Gateway";
    }
}
} // namespace
int main(int argc, char** argv)
{
    try
    {
        if (argc > 3)
            throw std::invalid_argument(
                "usage: rpc_gateway [port] [endpoint,endpoint]"
            );
        auto port = argc > 1 ? number(argv[1], 65535, true) : 8080;
        auto targets = endpoints(
            argc > 2 ? argv[2] : "127.0.0.1:50051"
        );
        auto limit = envNumber("SYLAR_RPC_MAX_INFLIGHT", 64, 4096);
        auto timeout = envNumber("SYLAR_RPC_TIMEOUT_MS", 500, 2000);
        auto retries = envNumber("SYLAR_RPC_RETRIES", 1, 2, true);
        auto trace_capacity = envNumber(
            "SYLAR_TRACE_CAPACITY",
            4096,
            65536,
            true
        );
        auto* secondary_targets = std::getenv(
            "SYLAR_RPC_SECONDARY_ENDPOINTS"
        );
        auto* zk_hosts = std::getenv("SYLAR_ZK_HOSTS");
        if (secondary_targets && limit < 2)
            throw std::invalid_argument(
                "two downstreams require capacity >= 2"
            );
#ifndef SYLAR_RPC_WITH_ZOOKEEPER
        if (zk_hosts)
            throw std::runtime_error(
                "rebuild with SYLAR_WITH_ZOOKEEPER"
            );
#endif
        std::string notification = std::getenv(
                                       "SYLAR_RPC_NOTIFICATION"
                                   )
                                       ? std::getenv(
                                             "SYLAR_RPC_NOTIFICATION"
                                         )
                                       : "worker";
        if (notification != "worker" && notification != "eventfd")
            throw std::invalid_argument("invalid notification mode");
        sylar::SignalFd signals(true);
        auto tracer = trace_capacity ? std::make_shared<
                                           sylar::rpc::TraceRecorder>(
                                           trace_capacity
                                       )
                                     : nullptr;
        sylar::rpc::GrpcRuntime runtime(
            limit,
            std::chrono::seconds(2),
            notification == "worker"
                ? sylar::rpc::GrpcRuntime::NotificationMode::
                      WorkerQueue
                : sylar::rpc::GrpcRuntime::NotificationMode::
                      PerCallEventfd,
            tracer
        );
        sylar::IOManager acceptor;
        sylar::IOManagerPool workers(2);
        sylar::rpc::CatalogClient::Options policy;
        policy.capacity = secondary_targets ? limit / 2 : limit;
        policy.max_retries = retries;
        policy.retry_budget = envNumber(
            "SYLAR_RPC_RETRY_BUDGET",
            4,
            1000,
            true
        );
        auto catalog = std::make_shared<sylar::rpc::CatalogClient>(
            runtime,
            zk_hosts ? std::vector<std::string>{} : targets,
            policy
        );
        std::shared_ptr<sylar::rpc::CatalogClient> secondary;
        if (secondary_targets)
        {
            auto second_policy = policy;
            second_policy.downstream = "secondary";
            second_policy.capacity = limit - policy.capacity;
            secondary = std::make_shared<sylar::rpc::CatalogClient>(
                runtime,
                endpoints(secondary_targets),
                second_policy
            );
        }
#ifdef SYLAR_RPC_WITH_ZOOKEEPER
        std::unique_ptr<sylar::rpc::ZkDiscovery> discovery;
        if (zk_hosts)
            discovery = sylar::rpc::ZkDiscovery::watch(
                zk_hosts,
                std::getenv("SYLAR_ZK_PATH")
                    ? std::getenv("SYLAR_ZK_PATH")
                    : "/sylar/services/catalog",
                [catalog](const auto& view)
                { catalog->updateEndpoints(view); }
            );
#endif
        auto dispatch = std::make_shared<sylar::ServletDispatch>();
        auto serial = std::make_shared<std::atomic_uint64_t>(0);
        dispatch->addExact(
            "/health",
            std::make_shared<sylar::FunctionServlet>(
                [](const auto&)
                {
                    sylar::HttpResponse response;
                    response.body = "ok\n";
                    return response;
                }
            )
        );
        auto addRoute =
            [&](std::string prefix,
                std::shared_ptr<sylar::rpc::CatalogClient> client)
        {
            dispatch->addPrefix(
                prefix,
                std::make_shared<sylar::FunctionServlet>(
                    [client, serial, timeout, tracer, prefix](
                        const sylar::HttpRequest& request
                    )
                    {
                        sylar::HttpResponse response;
                        response.headers["Content-Type"] =
                            "application/json";
                        if (request.method != "GET")
                        {
                            response.status = 405;
                            response.reason = "Method Not Allowed";
                            response.headers["Allow"] = "GET";
                            return response;
                        }
                        unsigned id, budget;
                        std::string request_id, parent;
                        try
                        {
                            id = number(
                                std::string_view(request.target)
                                    .substr(prefix.size()),
                                1000000
                            );
                            budget =
                                request.header("x-request-timeout-ms")
                                    ? number(
                                          *request.header(
                                              "x-request-timeout-ms"
                                          ),
                                          2000
                                      )
                                    : timeout;
                            request_id =
                                request.header("x-request-id")
                                    ? std::string(*request.header(
                                          "x-request-id"
                                      ))
                                    : "gw-" + std::to_string(
                                                  serial->fetch_add(1)
                                              );
                            if (!sylar::rpc::GrpcRuntime::
                                    validRequestId(request_id) ||
                                request_id.empty())
                                throw std::invalid_argument("bad ID");
                            if (auto header = request.header(
                                    "traceparent"
                                ))
                                parent = std::string(*header);
                            sylar::rpc::TraceContext::parse(parent);
                        }
                        catch (const std::invalid_argument&)
                        {
                            response.status = 400;
                            response.reason = reason(400);
                            response.body = "{\"error\":\"invalid "
                                            "request metadata\"}\n";
                            return response;
                        }
                        std::optional<sylar::rpc::TraceScope> span;
                        if (tracer || !parent.empty())
                            span.emplace(
                                tracer,
                                parent,
                                "GET " + prefix,
                                "http.server"
                            );
                        auto
                            deadline = std::chrono::steady_clock::now(
                                       ) +
                                       std::chrono::milliseconds(
                                           budget
                                       );
                        if (request.context &&
                            request.context->deadline())
                            deadline = std::min(
                                deadline,
                                *request.context->deadline()
                            );
                        sylar::RequestContext context(
                            deadline,
                            request_id,
                            span ? span->header() : ""
                        );
                        std::stop_callback cancel(
                            request.context ? request.context->token()
                                            : std::stop_token{},
                            [&] { context.cancel(); }
                        );
                        auto result = client->lookup(
                            *sylar::IOManager::GetThis(),
                            context,
                            id
                        );
                        response.status = sylar::rpc::httpStatus(
                            result.status.error_code()
                        );
                        response.reason = reason(response.status);
                        response.headers["X-Request-Id"] = request_id;
                        if (span)
                        {
                            span->finish(response.status);
                            response.headers["traceparent"] =
                                span->header();
                        }
                        if (result.status.ok())
                        {
                            auto& value = result.reply;
                            response.body = fmt::format(
                                "{{\"product_id\":{},\"name\":{},"
                                "\"price_cents\":{},\"instance_id\":{"
                                "},\"request_id\":{},\"trace_"
                                "parent\":{},\"server_elapsed_us\":{}"
                                "}}\n",
                                value.product_id(),
                                jsonString(value.name()),
                                value.price_cents(),
                                jsonString(value.instance_id()),
                                jsonString(value.request_id()),
                                jsonString(value.trace_parent()),
                                value.server_elapsed_us()
                            );
                        }
                        else
                            response.body = fmt::format(
                                "{{\"error\":\"upstream RPC "
                                "failed\",\"grpc_code\":{},\"request_"
                                "id\":{}}}\n",
                                static_cast<int>(
                                    result.status.error_code()
                                ),
                                jsonString(request_id)
                            );
                        return response;
                    }
                )
            );
        };
        addRoute("/products/", catalog);
        if (secondary)
            addRoute("/secondary/products/", secondary);
        dispatch->addExact(
            "/traces",
            std::make_shared<sylar::FunctionServlet>(
                [tracer](const auto&)
                {
                    sylar::HttpResponse response;
                    response
                        .headers["Content-Type"] = "application/json";
                    response.body =
                        tracer ? tracer->json()
                               : "{\"dropped\":0,\"spans\":[]}\n";
                    return response;
                }
            )
        );
        dispatch->addExact(
            "/endpoints",
            std::make_shared<sylar::FunctionServlet>(
                [catalog, secondary](const auto&)
                {
                    sylar::HttpResponse response;
                    response
                        .headers["Content-Type"] = "application/json";
                    response.body = "{\"endpoints\":[";
                    bool first = true;
                    for (auto& client : {catalog, secondary})
                        if (client)
                            for (auto& endpoint :
                                 client->endpointStatus())
                            {
                                if (!first)
                                    response.body += ',';
                                first = false;
                                response.body += fmt::format(
                                    "{{\"service\":{},\"address\":{},"
                                    "\"open\":{}}}",
                                    jsonString(
                                        client == catalog
                                            ? "catalog"
                                            : "secondary"
                                    ),
                                    jsonString(endpoint.address),
                                    endpoint.open ? "true" : "false"
                                );
                            }
                    response.body += "]}\n";
                    return response;
                }
            )
        );
        dispatch->addExact(
            "/metrics",
            std::make_shared<sylar::FunctionServlet>(
                [&runtime, catalog, secondary, tracer](const auto&)
                {
                    auto stats = runtime.snapshot();
                    sylar::HttpResponse response;
                    response.headers["Content-Type"] =
                        "text/plain; version=0.0.4";
                    response.body = fmt::format(
                        "sylar_rpc_inflight {}\nsylar_rpc_limit "
                        "{}\nsylar_rpc_started_total "
                        "{}\nsylar_rpc_completed_total "
                        "{}\nsylar_rpc_rejected_total "
                        "{}\nsylar_rpc_failed_total "
                        "{}\nsylar_rpc_deadlines_total "
                        "{}\nsylar_rpc_cancelled_total {}\n",
                        stats.in_flight,
                        stats.limit,
                        stats.started,
                        stats.completed,
                        stats.rejected,
                        stats.failed,
                        stats.deadlines,
                        stats.cancelled
                    );
                    for (auto& item : runtime.downstreamSnapshot())
                        response.body += fmt::format(
                            "sylar_rpc_downstream_inflight{{service="
                            "\"{}\"}} "
                            "{}\nsylar_rpc_downstream_limit{{service="
                            "\"{}\"}} "
                            "{}\nsylar_rpc_downstream_rejected_total{"
                            "{service=\"{}\"}} {}\n",
                            item.name,
                            item.in_flight,
                            item.name,
                            item.limit,
                            item.name,
                            item.rejected
                        );
                    for (auto& client : {catalog, secondary})
                        if (client)
                        {
                            auto retry = client->snapshot();
                            auto name = client == catalog
                                            ? "catalog"
                                            : "secondary";
                            response.body += fmt::format(
                                "sylar_rpc_retries_total{{service=\"{"
                                "}\"}} "
                                "{}\nsylar_rpc_retry_denied_total{{"
                                "service=\"{}\"}} {}\n",
                                name,
                                retry.retries,
                                name,
                                retry.retry_denied
                            );
                        }
                    response.body += fmt::format(
                        "sylar_trace_dropped_total {}\n",
                        tracer ? tracer->dropped() : 0
                    );
                    return response;
                }
            )
        );
        auto limiter = std::make_shared<sylar::RequestLimiter>(
            sylar::RequestLimiter::Options{
                .initial = 128,
                .minimum = 128,
                .maximum = 128}
        );
        auto server = std::make_shared<sylar::HttpServer>(
            acceptor,
            workers,
            dispatch,
            std::shared_ptr<sylar::TlsContext>{},
            limiter
        );
        server->setMaxConnections(512);
        std::weak_ptr<sylar::HttpServer> weak = server;
        dispatch->addExact(
            "/ready",
            std::make_shared<sylar::FunctionServlet>(
                [weak, catalog](const auto&)
                {
                    sylar::HttpResponse response;
                    auto owner = weak.lock();
                    auto view = catalog->endpointStatus();
                    bool healthy = std::any_of(
                        view.begin(),
                        view.end(),
                        [](auto& item) { return !item.open; }
                    );
                    response.status = owner && !owner->draining() &&
                                              healthy
                                          ? 200
                                          : 503;
                    response.reason = reason(response.status);
                    response.body = response.status == 200
                                        ? "ready\n"
                                        : "not ready\n";
                    return response;
                }
            )
        );
        server->start(
            std::getenv("SYLAR_LISTEN_ADDRESS")
                ? std::getenv("SYLAR_LISTEN_ADDRESS")
                : "127.0.0.1",
            port
        );
        std::cout << "rpc_gateway listening on port "
                  << server->localPort()
                  << " endpoints=" << targets.size() << std::endl;
        acceptor.schedule(
            [&]
            {
                while (acceptor.waitReadable(signals.get()) ==
                       sylar::IOManager::WaitResult::Ready)
                {
                    signalfd_siginfo info{};
                    if (::read(signals.get(), &info, sizeof(info)) !=
                        sizeof(info))
                        continue;
                    server->beginDrain();
                    if (info.ssi_signo == SIGUSR1)
                        continue;
                    server->stop();
                    auto deadline = std::chrono::steady_clock::now() +
                                    std::chrono::seconds(2);
                    while (server->activeConnections() &&
                           std::chrono::steady_clock::now() < deadline
                    )
                        acceptor.sleepFor(std::chrono::milliseconds(10
                        ));
                    acceptor.requestStop();
                    return;
                }
            }
        );
        acceptor.runForever();
        server->stop();
        workers.stop();
#ifdef SYLAR_RPC_WITH_ZOOKEEPER
        discovery.reset();
#endif
        runtime.shutdown();
        std::cout << "rpc_gateway stopped" << std::endl;
    }
    catch (const std::exception& error)
    {
        std::cerr << "rpc_gateway: " << error.what() << std::endl;
        return 1;
    }
}
