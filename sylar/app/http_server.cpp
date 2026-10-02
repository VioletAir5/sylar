#include <signal.h>
#include <sys/signalfd.h>
#include <unistd.h>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>

#include "os/SignalFd.hpp"

#include "fiber/IOManager.hpp"
#include "fiber/IOManagerPool.hpp"
#include "http/HttpServer.hpp"
#include "http/Servlet.hpp"

namespace
{

unsigned parsePositive(std::string_view text)
{
    unsigned result = 0;
    const auto [end, error] = std::from_chars(
        text.data(),
        text.data() + text.size(),
        result
    );
    if (error != std::errc{} || end != text.data() + text.size() ||
        result == 0)
    {
        throw std::invalid_argument("expected positive number");
    }
    return result;
}

unsigned envPositive(const char* name, unsigned fallback)
{
    const char* value = std::getenv(name);
    return value && *value ? parsePositive(value) : fallback;
}

std::string instanceId()
{
    const char* value = std::getenv("SYLAR_INSTANCE_ID");
    if (!value || !*value)
    {
        value = std::getenv("HOSTNAME");
    }
    if (!value || !*value)
    {
        return "local";
    }
    const std::string id(value);
    if (id.size() > 128 || !std::all_of(
                               id.begin(),
                               id.end(),
                               [](unsigned char ch)
                               {
                                   return (ch >= 'a' && ch <= 'z') ||
                                          (ch >= 'A' && ch <= 'Z') ||
                                          (ch >= '0' && ch <= '9') ||
                                          ch == '-' || ch == '_' ||
                                          ch == '.';
                               }
                           ))
    {
        throw std::invalid_argument("invalid instance ID");
    }
    return id;
}

std::shared_ptr<sylar::ServletDispatch>
routes(std::string instance_id, unsigned demo_work_ms)
{
    auto dispatch = std::make_shared<sylar::ServletDispatch>();
    dispatch->addExact(
        "/",
        std::make_shared<sylar::FunctionServlet>(
            [](const sylar::HttpRequest&)
            {
                sylar::HttpResponse response;
                response.headers["Content-Type"] =
                    "text/plain; charset=utf-8";
                response.body = "sylar HTTP server\n";
                return response;
            }
        )
    );
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
    if (demo_work_ms != 0)
    {
        dispatch->addExact(
            "/work",
            std::make_shared<sylar::FunctionServlet>(
                [demo_work_ms](const sylar::HttpRequest&)
                {
                    sylar::IOManager::GetThis()->sleepFor(
                        std::chrono::milliseconds(demo_work_ms)
                    );
                    sylar::HttpResponse response;
                    response.body = "done\n";
                    return response;
                }
            )
        );
    }
    dispatch->addExact(
        "/instance",
        std::make_shared<sylar::FunctionServlet>(
            [instance_id = std::move(instance_id
             )](const sylar::HttpRequest&)
            {
                sylar::HttpResponse response;
                response.headers["Content-Type"] = "text/plain";
                response.headers["Cache-Control"] = "no-store";
                response.body = instance_id + "\n";
                return response;
            }
        )
    );
    dispatch->addExact(
        "/echo",
        std::make_shared<sylar::FunctionServlet>(
            [](const sylar::HttpRequest& request)
            {
                sylar::HttpResponse response;
                response.headers["Content-Type"] =
                    "application/octet-stream";
                response.body = request.body;
                return response;
            }
        )
    );
    return dispatch;
}

} // namespace

int main(int argc, char** argv)
{
    try
    {
        const bool tls_mode = argc == 6 &&
                              std::string_view(argv[3]) == "--tls";
        if (argc > 3 && !tls_mode)
        {
            throw std::invalid_argument(
                "usage: http_server [port] [workers] "
                "[--tls cert.pem key.pem]"
            );
        }
        const auto port = argc > 1 ? parsePositive(argv[1]) : 8080;
        const auto workers = argc > 2 ? parsePositive(argv[2]) : 4;
        if (port > 65535 || workers > 128)
        {
            throw std::invalid_argument("invalid port or worker count"
            );
        }

        const auto max_connections = envPositive(
            "SYLAR_MAX_CONNECTIONS",
            4096
        );
        const auto max_requests = envPositive(
            "SYLAR_MAX_INFLIGHT_REQUESTS",
            1024
        );
        const auto demo_work_ms = envPositive(
            "SYLAR_DEMO_WORK_MS",
            0
        );
        const char* adaptive_value = std::getenv(
            "SYLAR_ADAPTIVE_CONCURRENCY"
        );
        const bool adaptive = adaptive_value &&
                              std::string_view(adaptive_value) == "1";
        if (adaptive_value &&
            std::string_view(adaptive_value) != "0" &&
            std::string_view(adaptive_value) != "1")
        {
            throw std::invalid_argument(
                "invalid adaptive concurrency flag"
            );
        }
        const auto drain_timeout_ms = envPositive(
            "SYLAR_DRAIN_TIMEOUT_MS",
            10000
        );
        if (max_connections > 1000000 || max_requests > 1000000 ||
            drain_timeout_ms > 60000 || demo_work_ms > 1000)
        {
            throw std::invalid_argument(
                "invalid connection limit or drain timeout"
            );
        }

        sylar::SignalFd signals(true);
        sylar::IOManager io;
        sylar::IOManagerPool pool(workers);
        auto tls = tls_mode
                       ? sylar::TlsContext::server(argv[4], argv[5])
                       : std::shared_ptr<sylar::TlsContext>{};
        const auto instance_id = instanceId();
        auto dispatch = routes(instance_id, demo_work_ms);
        auto limiter = std::make_shared<sylar::RequestLimiter>(
            sylar::RequestLimiter::Options{
                .initial = max_requests,
                .minimum = adaptive ? std::max(1U, max_requests / 4)
                                    : max_requests,
                .maximum = max_requests,
                .window = 64,
                .adaptive = adaptive,
            }
        );
        auto server = std::make_shared<sylar::HttpServer>(
            io,
            pool,
            dispatch,
            std::move(tls),
            std::move(limiter)
        );
        server->setMaxConnections(max_connections);
        std::weak_ptr<sylar::HttpServer> server_weak = server;
        dispatch->addExact(
            "/ready",
            std::make_shared<sylar::FunctionServlet>(
                [server_weak](const sylar::HttpRequest&)
                {
                    sylar::HttpResponse response;
                    const auto current = server_weak.lock();
                    if (!current || current->draining())
                    {
                        response.status = 503;
                        response.reason = "Service Unavailable";
                        response.body = "draining\n";
                    }
                    else
                    {
                        response.body = "ready\n";
                    }
                    return response;
                }
            )
        );
        dispatch->addExact(
            "/metrics",
            std::make_shared<sylar::FunctionServlet>(
                [server_weak, &pool](const sylar::HttpRequest&)
                {
                    sylar::HttpResponse response;
                    const auto current = server_weak.lock();
                    if (!current)
                    {
                        response.status = 503;
                        response.reason = "Service Unavailable";
                        return response;
                    }
                    response.headers["Content-Type"] =
                        "text/plain; version=0.0.4";
                    response.body =
                        "# TYPE sylar_tcp_active_connections gauge\n"
                        "sylar_tcp_active_connections " +
                        std::to_string(current->activeConnections()) +
                        "\n# TYPE "
                        "sylar_tcp_rejected_connections_total "
                        "counter\n"
                        "sylar_tcp_rejected_connections_total " +
                        std::to_string(current->rejectedConnections()
                        ) +
                        "\n# TYPE sylar_tcp_connection_limit gauge\n"
                        "sylar_tcp_connection_limit " +
                        std::to_string(current->maxConnections()) +
                        "\n";
                    const auto request_stats = current->requestStats(
                    );
                    response.body +=
                        "# TYPE sylar_http_inflight_requests gauge\n"
                        "sylar_http_inflight_requests " +
                        std::to_string(request_stats.in_flight) +
                        "\n# TYPE sylar_http_request_limit gauge\n"
                        "sylar_http_request_limit " +
                        std::to_string(request_stats.limit) +
                        "\n# TYPE sylar_http_rejected_requests_total "
                        "counter\n"
                        "sylar_http_rejected_requests_total " +
                        std::to_string(request_stats.rejected) +
                        "\n# TYPE "
                        "sylar_http_completed_requests_total "
                        "counter\n"
                        "sylar_http_completed_requests_total " +
                        std::to_string(request_stats.completed) +
                        "\nsylar_http_adaptive_concurrency_enabled " +
                        std::to_string(
                            request_stats.adaptive ? 1 : 0
                        ) +
                        "\n";
                    const auto outbound = sylar::OutboundBudget::
                        processDefault();
                    response.body +=
                        "# TYPE sylar_ws_outbound_bytes gauge\n"
                        "sylar_ws_outbound_bytes " +
                        std::to_string(outbound->inFlightBytes()) +
                        "\n# TYPE sylar_ws_outbound_limit_bytes "
                        "gauge\n"
                        "sylar_ws_outbound_limit_bytes " +
                        std::to_string(outbound->maximumBytes()) +
                        "\n# TYPE sylar_ws_outbound_rejected_total "
                        "counter\n"
                        "sylar_ws_outbound_rejected_total " +
                        std::to_string(outbound->rejected()) + "\n";
                    const auto workers = pool.diagnostics();
                    for (std::size_t index = 0;
                         index < workers.size();
                         ++index)
                    {
                        const auto& stats = workers[index];
                        const auto label = "{worker=\"" +
                                           std::to_string(index) +
                                           "\"} ";
                        const auto append = [&](std::string_view name,
                                                std::uint64_t value)
                        {
                            response.body += std::string(name) +
                                             label +
                                             std::to_string(value) +
                                             "\n";
                        };
                        append(
                            "sylar_io_diagnostics_enabled",
                            stats.enabled ? 1 : 0
                        );
                        append("sylar_io_posts_total", stats.posts);
                        append(
                            "sylar_io_post_queue",
                            stats.post_queue
                        );
                        append(
                            "sylar_io_waiter_registrations_total",
                            stats.waiter_registrations
                        );
                        append(
                            "sylar_io_epoll_ctl_calls_total",
                            stats.epoll_ctl_calls
                        );
                        append(
                            "sylar_io_epoll_wait_calls_total",
                            stats.epoll_wait_calls
                        );
                        append(
                            "sylar_io_epoll_wait_nanoseconds_total",
                            stats.epoll_wait_ns
                        );
                        append(
                            "sylar_io_epoll_ready_events_total",
                            stats.epoll_ready_events
                        );
                        append(
                            "sylar_io_eventfd_wakeups_total",
                            stats.eventfd_wakeups
                        );
                    }
                    return response;
                }
            )
        );
        server->addWebSocket(
            "/ws",
            [](sylar::WebSocketSession& session,
               const sylar::WebSocketMessage& message)
            {
                if (message.type ==
                    sylar::WebSocketMessage::Type::Text)
                {
                    session.sendText(message.payload);
                }
                else
                {
                    session.sendBinary(message.payload);
                }
            }
        );
        server->start("0.0.0.0", static_cast<std::uint16_t>(port));
        std::cout << (tls_mode ? "https_server" : "http_server")
                  << " listening on port " << server->localPort()
                  << " workers=" << workers
                  << " instance=" << instance_id
                  << " max_connections=" << max_connections
                  << " max_requests=" << max_requests
                  << " adaptive=" << (adaptive ? 1 : 0) << std::endl;
        io.schedule(
            [&]
            {
                while (io.waitReadable(signals.get()) ==
                       sylar::IOManager::WaitResult::Ready)
                {
                    signalfd_siginfo info{};
                    if (::read(signals.get(), &info, sizeof(info)) ==
                        sizeof(info))
                    {
                        server->beginDrain();
                        if (info.ssi_signo == SIGUSR1)
                        {
                            std::cout << "http_server draining"
                                      << std::endl;
                            continue;
                        }
                        server->stop();
                        const auto
                            deadline = std::chrono::steady_clock::now(
                                       ) +
                                       std::chrono::milliseconds(
                                           drain_timeout_ms
                                       );
                        while (server->activeConnections() != 0 &&
                               std::chrono::steady_clock::now() <
                                   deadline)
                        {
                            io.sleepFor(std::chrono::milliseconds(25)
                            );
                        }
                        io.requestStop();
                        return;
                    }
                }
            }
        );
        io.runForever();
        server->stop();
        pool.stop();
        std::cout << "http_server stopped" << std::endl;
    }
    catch (const std::exception& error)
    {
        std::cerr << "http_server: " << error.what() << std::endl;
        return 1;
    }
}
