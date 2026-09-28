#include <chrono>
#include <memory>
#include <span>
#include <string>

#include <gtest/gtest.h>

#include "net/Address.hpp"
#include "net/Socket.hpp"
#include "net/SocketStream.hpp"

#include "fiber/IOManager.hpp"
#include "http/HttpServer.hpp"
#include "http/Servlet.hpp"

using namespace std::chrono_literals;

TEST(ServletTest, RoutesExactPrefixAndFallback)
{
    sylar::ServletDispatch routes;
    routes.addPrefix(
        "/api",
        std::make_shared<sylar::FunctionServlet>(
            [](const sylar::HttpRequest&)
            {
                sylar::HttpResponse response;
                response.body = "prefix";
                return response;
            }
        )
    );
    routes.addExact(
        "/api/item",
        std::make_shared<sylar::FunctionServlet>(
            [](const sylar::HttpRequest&)
            {
                sylar::HttpResponse response;
                response.body = "exact";
                return response;
            }
        )
    );
    sylar::HttpRequest request;
    request.target = "/api/item?id=1";
    EXPECT_EQ(routes.handle(request).body, "exact");
    request.target = "/api/other";
    EXPECT_EQ(routes.handle(request).body, "prefix");
    request.target = "/missing";
    EXPECT_EQ(routes.handle(request).status, 404);
}

TEST(HttpServerTest, HandlesTwoRequestsOnOneConnection)
{
    sylar::IOManager io;
    auto routes = std::make_shared<sylar::ServletDispatch>();
    routes->addExact(
        "/hello",
        std::make_shared<sylar::FunctionServlet>(
            [](const sylar::HttpRequest&)
            {
                sylar::HttpResponse response;
                response.body = "hello";
                return response;
            }
        )
    );
    sylar::HttpServer server(io, routes);
    server.start("127.0.0.1", 0);
    const auto port = server.localPort();
    std::string wire;

    io.schedule(
        [&]
        {
            auto socket = sylar::Socket::tcp();
            socket.connect(
                io,
                sylar::Address::ipv4("127.0.0.1", port),
                1s
            );
            sylar::SocketStream stream(io, std::move(socket), 1s);
            const std::string
                requests = "GET /hello HTTP/1.1\r\nHost: test\r\n\r\n"
                           "GET /missing HTTP/1.1\r\nHost: test\r\n"
                           "Connection: close\r\n\r\n";
            stream.writeAll(std::as_bytes(std::span(requests)));
            std::byte buffer[512];
            for (;;)
            {
                const auto count = stream.readSome(buffer);
                if (count == 0)
                {
                    break;
                }
                wire.append(
                    reinterpret_cast<const char*>(buffer),
                    count
                );
            }
            server.stop();
        }
    );
    io.run();
    EXPECT_NE(wire.find("HTTP/1.1 200 OK"), std::string::npos);
    EXPECT_NE(wire.find("hello"), std::string::npos);
    EXPECT_NE(wire.find("HTTP/1.1 404 Not Found"), std::string::npos);
}
