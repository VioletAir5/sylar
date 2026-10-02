#include <sys/signalfd.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

#include <fmt/format.h>

#include "os/SignalFd.hpp"

#include "fiber/IOManagerPool.hpp"
#include "http/HttpServer.hpp"
#include "rpc/CatalogClient.hpp"

namespace
{
unsigned number(
    std::string_view text, unsigned maximum, bool allow_zero = false
)
{
    unsigned value = 0;
    const auto [end, error] = std::from_chars(
        text.data(),
        text.data() + text.size(),
        value
    );
    if (error != std::errc{} || end != text.data() + text.size() ||
        value > maximum || (!allow_zero && value == 0))
    {
        throw std::invalid_argument("invalid numeric argument");
    }
    return value;
}

unsigned
envNumber(const char* name, unsigned fallback, unsigned maximum)
{
    const auto* value = std::getenv(name);
    return value && *value ? number(value, maximum) : fallback;
}

std::vector<std::string> endpoints(std::string_view text)
{
    std::vector<std::string> result;
    do
    {
        const auto comma = text.find(',');
        auto endpoint = text.substr(0, comma);
        if (endpoint.empty())
        {
            throw std::invalid_argument("empty RPC endpoint");
        }
        result.emplace_back(endpoint);
        if (comma == std::string_view::npos)
        {
            break;
        }
        text.remove_prefix(comma + 1);
    } while (true);
    return result;
}

std::string jsonString(std::string_view value)
{
    std::string result = "\"";
    for (unsigned char ch : value)
    {
        if (ch == '"' || ch == '\\')
        {
            result.push_back('\\');
            result.push_back(static_cast<char>(ch));
        }
        else if (ch < 32)
        {
            result += fmt::format("\\u{:04x}", ch);
        }
        else
        {
            result.push_back(static_cast<char>(ch));
        }
    }
    result += '"';
    return result;
}

const char* reason(int status)
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
        {
            throw std::invalid_argument(
                "usage: rpc_gateway [port] [endpoint,endpoint]"
            );
        }
        const auto port = argc > 1 ? number(argv[1], 65535, true)
                                   : 8080;
        const auto targets = endpoints(
            argc > 2 ? argv[2] : "127.0.0.1:50051"
        );
        const auto limit = envNumber(
            "SYLAR_RPC_MAX_INFLIGHT",
            64,
            4096
        );
        const auto default_timeout = envNumber(
            "SYLAR_RPC_TIMEOUT_MS",
            500,
            2000
        );
        sylar::SignalFd signals(true);
        sylar::rpc::GrpcRuntime runtime(limit);
        sylar::IOManager acceptor;
        sylar::IOManagerPool workers(2);
        auto catalog = std::make_shared<sylar::rpc::CatalogClient>(
            runtime,
            targets
        );
        auto dispatch = std::make_shared<sylar::ServletDispatch>();
        auto serial = std::make_shared<std::atomic_uint64_t>(0);
        dispatch->addExact(
            "/health",
            std::make_shared<sylar::FunctionServlet>(
                [](const sylar::HttpRequest&)
                {
                    sylar::HttpResponse response;
                    response.body = "ok\n";
                    return response;
                }
            )
        );
        dispatch->addPrefix(
            "/products/",
            std::make_shared<sylar::FunctionServlet>(
                [catalog,
                 serial,
                 default_timeout](const sylar::HttpRequest& request)
                {
                    sylar::HttpResponse response;
                    response
                        .headers["Content-Type"] = "application/json";
                    if (request.method != "GET")
                    {
                        response.status = 405;
                        response.reason = "Method Not Allowed";
                        response.headers["Allow"] = "GET";
                        return response;
                    }
                    unsigned id;
                    unsigned timeout;
                    std::string request_id;
                    try
                    {
                        id = number(
                            std::string_view(request.target)
                                .substr(10),
                            1000000
                        );
                        timeout = request.header(
                                      "x-request-timeout-ms"
                                  )
                                      ? number(
                                            *request.header(
                                                "x-request-timeout-ms"
                                            ),
                                            2000
                                        )
                                      : default_timeout;
                        request_id = request.header("x-request-id")
                                         ? std::string(
                                               *request.header(
                                                   "x-request-id"
                                               )
                                           )
                                         : "gw-" +
                                               std::to_string(
                                                   serial->fetch_add(1
                                                   )
                                               );
                        if (!sylar::rpc::GrpcRuntime::validRequestId(
                                request_id
                            ) ||
                            request_id.empty())
                        {
                            throw std::invalid_argument(
                                "invalid request id"
                            );
                        }
                    }
                    catch (const std::invalid_argument&)
                    {
                        response.status = 400;
                        response.reason = reason(400);
                        response
                            .body = "{\"error\":\"invalid product, "
                                    "timeout or request ID\"}\n";
                        return response;
                    }
                    auto deadline = std::chrono::steady_clock::now() +
                                    std::chrono::milliseconds(timeout
                                    );
                    if (request.context &&
                        request.context->deadline())
                    {
                        deadline = std::min(
                            deadline,
                            *request.context->deadline()
                        );
                    }
                    sylar::RequestContext context(
                        deadline,
                        request_id
                    );
                    std::stop_callback propagate(
                        request.context ? request.context->token()
                                        : std::stop_token{},
                        [&context] { context.cancel(); }
                    );
                    auto result = catalog->lookup(
                        *sylar::IOManager::GetThis(),
                        context,
                        id
                    );
                    response.status = sylar::rpc::httpStatus(
                        result.status.error_code()
                    );
                    response.reason = reason(response.status);
                    response.headers["X-Request-Id"] = request_id;
                    if (result.status.ok())
                    {
                        const auto& product = result.reply;
                        response.body = fmt::format(
                            "{{\"product_id\":{},\"name\":{},\"price_"
                            "cents\":{},\"instance_id\":{},\"request_"
                            "id\":{}}}\n",
                            product.product_id(),
                            jsonString(product.name()),
                            product.price_cents(),
                            jsonString(product.instance_id()),
                            jsonString(product.request_id())
                        );
                    }
                    else
                    {
                        response.body = fmt::format(
                            "{{\"error\":\"upstream RPC "
                            "failed\",\"grpc_code\":{},\"request_"
                            "id\":{}}}\n",
                            static_cast<int>(result.status.error_code(
                            )),
                            jsonString(request_id)
                        );
                    }
                    return response;
                }
            )
        );
        dispatch->addExact(
            "/metrics",
            std::make_shared<sylar::FunctionServlet>(
                [&runtime](const sylar::HttpRequest&)
                {
                    const auto stats = runtime.snapshot();
                    sylar::HttpResponse response;
                    response.headers["Content-Type"] =
                        "text/plain; version=0.0.4";
                    response.body = fmt::format(
                        "sylar_rpc_inflight {}\nsylar_rpc_limit {}\n"
                        "sylar_rpc_started_total "
                        "{}\nsylar_rpc_completed_total {}\n"
                        "sylar_rpc_rejected_total "
                        "{}\nsylar_rpc_failed_total {}\n"
                        "sylar_rpc_deadlines_total "
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
                [weak](const sylar::HttpRequest&)
                {
                    sylar::HttpResponse response;
                    auto current = weak.lock();
                    response.status = current && !current->draining()
                                          ? 200
                                          : 503;
                    response.reason = reason(response.status);
                    response.body = response.status == 200
                                        ? "ready\n"
                                        : "draining\n";
                    return response;
                }
            )
        );
        server->start(
            (std::getenv("SYLAR_LISTEN_ADDRESS")
                 ? std::getenv("SYLAR_LISTEN_ADDRESS")
                 : "127.0.0.1"),
            static_cast<std::uint16_t>(port)
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
                    {
                        continue;
                    }
                    server->beginDrain();
                    if (info.ssi_signo == SIGUSR1)
                    {
                        continue;
                    }
                    server->stop();
                    const auto
                        deadline = std::chrono::steady_clock::now() +
                                   std::chrono::seconds(2);
                    while (server->activeConnections() != 0 &&
                           std::chrono::steady_clock::now() < deadline
                    )
                    {
                        acceptor.sleepFor(std::chrono::milliseconds(10
                        ));
                    }
                    acceptor.requestStop();
                    return;
                }
            }
        );
        acceptor.runForever();
        server->stop();
        workers.stop();
        runtime.shutdown();
        std::cout << "rpc_gateway stopped" << std::endl;
    }
    catch (const std::exception& error)
    {
        std::cerr << "rpc_gateway: " << error.what() << std::endl;
        return 1;
    }
}
