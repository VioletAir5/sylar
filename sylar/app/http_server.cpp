#include <sys/signalfd.h>
#include <unistd.h>

#include <charconv>
#include <cstdint>
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

std::shared_ptr<sylar::ServletDispatch> routes()
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

        sylar::SignalFd signals;
        sylar::IOManager io;
        sylar::IOManagerPool pool(workers);
        auto tls = tls_mode
                       ? sylar::TlsContext::server(argv[4], argv[5])
                       : std::shared_ptr<sylar::TlsContext>{};
        sylar::HttpServer server(io, pool, routes(), std::move(tls));
        server.start("0.0.0.0", static_cast<std::uint16_t>(port));
        std::cout << (tls_mode ? "https_server" : "http_server")
                  << " listening on port " << server.localPort()
                  << " workers=" << workers << std::endl;
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
                        server.stop();
                        io.requestStop();
                        return;
                    }
                }
            }
        );
        io.runForever();
        server.stop();
        pool.stop();
        std::cout << "http_server stopped" << std::endl;
    }
    catch (const std::exception& error)
    {
        std::cerr << "http_server: " << error.what() << std::endl;
        return 1;
    }
}
