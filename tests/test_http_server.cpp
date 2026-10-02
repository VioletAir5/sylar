#include <atomic>
#include <chrono>
#include <memory>
#include <span>
#include <string>

#include <gtest/gtest.h>

#include "net/Address.hpp"
#include "net/Socket.hpp"
#include "net/SocketStream.hpp"
#include "net/TcpServer.hpp"

#include "fiber/IOManager.hpp"
#include "fiber/IOManagerPool.hpp"
#include "fiber/RequestContext.hpp"
#include "http/HttpClient.hpp"
#include "http/HttpServer.hpp"
#include "http/Servlet.hpp"
#include "http/Uri.hpp"

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
    bool context_seen = false;
    routes->addExact(
        "/hello",
        std::make_shared<sylar::FunctionServlet>(
            [&](const sylar::HttpRequest& request)
            {
                context_seen = request.context &&
                               request.context->deadline().has_value(
                               );
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
    EXPECT_TRUE(context_seen);
    EXPECT_NE(wire.find("HTTP/1.1 200 OK"), std::string::npos);
    EXPECT_NE(wire.find("hello"), std::string::npos);
    EXPECT_NE(wire.find("HTTP/1.1 404 Not Found"), std::string::npos);
}

TEST(UriTest, ParsesAuthorityAndTarget)
{
    const auto uri = sylar::Uri::parse(
        "http://127.0.0.1:8080/search?q=hello#part"
    );
    EXPECT_EQ(uri.scheme, "http");
    EXPECT_EQ(uri.host, "127.0.0.1");
    EXPECT_EQ(uri.port, 8080);
    EXPECT_EQ(uri.authority(), "127.0.0.1:8080");
    EXPECT_EQ(uri.target, "/search?q=hello");
    const auto ipv6 = sylar::Uri::parse("http://[::1]:9000/");
    EXPECT_EQ(ipv6.host, "::1");
    EXPECT_EQ(ipv6.authority(), "[::1]:9000");
    EXPECT_THROW(
        sylar::Uri::parse("http://host:99999/"),
        std::invalid_argument
    );
    EXPECT_THROW(
        sylar::Uri::parse(
            std::string("http://host/") + '\0' + "path"
        ),
        std::invalid_argument
    );
    EXPECT_THROW(
        sylar::Uri::parse(std::string("http://ho") + '\0' + "st/"),
        std::invalid_argument
    );
}

TEST(HttpClientTest, RequestsLocalServlet)
{
    sylar::IOManager io;
    auto routes = std::make_shared<sylar::ServletDispatch>();
    routes->addExact(
        "/echo",
        std::make_shared<sylar::FunctionServlet>(
            [](const sylar::HttpRequest& request)
            {
                sylar::HttpResponse response;
                response.body = request.body;
                return response;
            }
        )
    );
    sylar::HttpServer server(io, routes);
    server.start("127.0.0.1", 0);
    sylar::HttpResponse response;
    io.schedule(
        [&]
        {
            const auto uri = sylar::Uri::parse(
                "http://127.0.0.1:" +
                std::to_string(server.localPort()) + "/echo"
            );
            response = sylar::HttpClient::request(
                io,
                uri,
                "POST",
                "client-body"
            );
            server.stop();
        }
    );
    io.run();
    EXPECT_EQ(response.status, 200);
    EXPECT_EQ(response.body, "client-body");
}

TEST(HttpClientTest, DecodesChunkedResponse)
{
    sylar::IOManager io;
    sylar::TcpServer server(
        io,
        [](sylar::IOManager& manager, int fd)
        {
            sylar::SocketStream stream(
                manager,
                sylar::Socket::borrow(fd)
            );
            const std::string wire = "HTTP/1.1 200 OK\r\n"
                                     "Transfer-Encoding: chunked\r\n"
                                     "Connection: close\r\n\r\n"
                                     "5\r\nhello\r\n"
                                     "6\r\n world\r\n"
                                     "0\r\nX-Checksum: yes\r\n\r\n";
            stream.writeAll(std::as_bytes(std::span(wire)));
        }
    );
    server.start("127.0.0.1", 0);
    sylar::HttpResponse response;
    io.schedule(
        [&]
        {
            const auto uri = sylar::Uri::parse(
                "http://127.0.0.1:" +
                std::to_string(server.localPort()) + "/"
            );
            response = sylar::HttpClient::request(io, uri);
            server.stop();
        }
    );
    io.run();
    EXPECT_EQ(response.body, "hello world");
}

TEST(HttpServerTest, UpgradesWebSocketAndKeepsHttpRouteOnSamePort)
{
    sylar::IOManager io;
    auto routes = std::make_shared<sylar::ServletDispatch>();
    routes->addExact(
        "/health",
        std::make_shared<sylar::FunctionServlet>(
            [](const sylar::HttpRequest&)
            {
                sylar::HttpResponse response;
                response.body = "ok";
                return response;
            }
        )
    );
    sylar::HttpServer server(io, routes);
    server.addWebSocket(
        "/ws",
        [](sylar::WebSocketSession& session,
           const sylar::WebSocketMessage& message)
        { session.sendText(message.payload); }
    );
    EXPECT_THROW(server.addWebSocket("", {}), std::invalid_argument);
    server.start("127.0.0.1", 0);
    std::string websocket_wire;
    sylar::HttpResponse http_response;
    io.schedule(
        [&]
        {
            {
                auto socket = sylar::Socket::tcp();
                socket.connect(
                    io,
                    sylar::Address::ipv4(
                        "127.0.0.1",
                        server.localPort()
                    ),
                    5s
                );
                sylar::SocketStream stream(io, std::move(socket), 5s);
                std::string request =
                    "GET /ws?room=1 HTTP/1.1\r\n"
                    "Host: localhost\r\n"
                    "Upgrade: websocket\r\n"
                    "Connection: Upgrade\r\n"
                    "Sec-WebSocket-Version: 13\r\n"
                    "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
                    "\r\n";
                request.append(
                    {static_cast<char>(0x81),
                     static_cast<char>(0x82),
                     1,
                     2,
                     3,
                     4,
                     static_cast<char>('h' ^ 1),
                     static_cast<char>('i' ^ 2)}
                );
                stream.writeAll(std::as_bytes(std::span(request)));
                const std::string frame{
                    static_cast<char>(0x81),
                    2,
                    'h',
                    'i'};
                std::byte buffer[512];
                while (websocket_wire.find(frame) == std::string::npos
                )
                {
                    const auto count = stream.readSome(buffer);
                    if (count == 0)
                    {
                        break;
                    }
                    websocket_wire.append(
                        reinterpret_cast<const char*>(buffer),
                        count
                    );
                }
            }
            const auto uri = sylar::Uri::parse(
                "http://127.0.0.1:" +
                std::to_string(server.localPort()) + "/health"
            );
            http_response = sylar::HttpClient::request(io, uri);
            server.stop();
        }
    );
    io.run();
    EXPECT_NE(
        websocket_wire.find("101 Switching Protocols"),
        std::string::npos
    );
    EXPECT_NE(
        websocket_wire.find(
            std::string{static_cast<char>(0x81), 2, 'h', 'i'}
        ),
        std::string::npos
    );
    EXPECT_EQ(http_response.status, 200);
    EXPECT_EQ(http_response.body, "ok");
}

TEST(HttpServerTest, HeadReturnsRepresentationLengthWithoutBody)
{
    sylar::IOManager io;
    auto routes = std::make_shared<sylar::ServletDispatch>();
    routes->addExact(
        "/resource",
        std::make_shared<sylar::FunctionServlet>(
            [](const sylar::HttpRequest&)
            {
                sylar::HttpResponse response;
                response.body = "content";
                return response;
            }
        )
    );
    sylar::HttpServer server(io, routes);
    server.start("127.0.0.1", 0);
    sylar::HttpResponse response;
    io.schedule(
        [&]
        {
            const auto uri = sylar::Uri::parse(
                "http://127.0.0.1:" +
                std::to_string(server.localPort()) + "/resource"
            );
            response = sylar::HttpClient::request(io, uri, "HEAD");
            server.stop();
        }
    );
    io.run();
    EXPECT_EQ(response.status, 200);
    EXPECT_EQ(response.body, "");
    EXPECT_EQ(response.headers.at("content-length"), "7");
}

TEST(HttpClientTest, ReadsFinalResponseAfterInformationalHeads)
{
    sylar::IOManager io;
    sylar::TcpServer server(
        io,
        [](sylar::IOManager& manager, int fd)
        {
            sylar::SocketStream stream(
                manager,
                sylar::Socket::borrow(fd)
            );
            const std::string wire =
                "HTTP/1.1 103 Early Hints\r\n"
                "Link: </style.css>; rel=preload\r\n\r\n"
                "HTTP/1.1 100 Continue\r\n\r\n"
                "HTTP/1.1 200 OK\r\n"
                "Content-Length: 5\r\nConnection: close\r\n\r\n"
                "final";
            stream.writeAll(std::as_bytes(std::span(wire)));
        }
    );
    server.start("127.0.0.1", 0);
    sylar::HttpResponse response;
    io.schedule(
        [&]
        {
            const auto uri = sylar::Uri::parse(
                "http://127.0.0.1:" +
                std::to_string(server.localPort()) + "/"
            );
            response = sylar::HttpClient::request(io, uri);
            server.stop();
        }
    );
    io.run();
    EXPECT_EQ(response.status, 200);
    EXPECT_EQ(response.body, "final");
}

TEST(HttpClientTest, RejectsInvalidRequestMetadataBeforeConnect)
{
    sylar::IOManager io;
    const auto uri = sylar::Uri::parse("http://127.0.0.1:1/");
    EXPECT_THROW(
        sylar::HttpClient::request(
            io,
            uri,
            std::string("GET") + '\0'
        ),
        std::invalid_argument
    );
    EXPECT_THROW(
        sylar::HttpClient::request(
            io,
            uri,
            "GET",
            {},
            {{"X-Test", std::string("bad") + '\0'}}
        ),
        std::invalid_argument
    );
}

TEST(HttpClientTest, RejectsInvalidResponseHeader)
{
    sylar::IOManager io;
    sylar::TcpServer server(
        io,
        [](sylar::IOManager& manager, int fd)
        {
            sylar::SocketStream stream(
                manager,
                sylar::Socket::borrow(fd)
            );
            const std::string
                wire = std::string("HTTP/1.1 200 OK\r\nX-Test: bad") +
                       '\0' + "\r\nContent-Length: 0\r\n\r\n";
            stream.writeAll(std::as_bytes(std::span(wire)));
        }
    );
    server.start("127.0.0.1", 0);
    io.schedule(
        [&]
        {
            const auto uri = sylar::Uri::parse(
                "http://127.0.0.1:" +
                std::to_string(server.localPort()) + "/"
            );
            EXPECT_THROW(
                sylar::HttpClient::request(io, uri),
                std::invalid_argument
            );
            server.stop();
        }
    );
    io.run();
}

TEST(HttpClientTest, RejectsInvalidChunkedTrailer)
{
    sylar::IOManager io;
    sylar::TcpServer server(
        io,
        [](sylar::IOManager& manager, int fd)
        {
            sylar::SocketStream stream(
                manager,
                sylar::Socket::borrow(fd)
            );
            const std::string
                wire = "HTTP/1.1 200 OK\r\n"
                       "Transfer-Encoding: chunked\r\n\r\n"
                       "0\r\nBad Name: value\r\n\r\n";
            stream.writeAll(std::as_bytes(std::span(wire)));
        }
    );
    server.start("127.0.0.1", 0);
    io.schedule(
        [&]
        {
            const auto uri = sylar::Uri::parse(
                "http://127.0.0.1:" +
                std::to_string(server.localPort()) + "/"
            );
            EXPECT_THROW(
                sylar::HttpClient::request(io, uri),
                std::invalid_argument
            );
            server.stop();
        }
    );
    io.run();
}

TEST(HttpServerTest, RejectsOverlappingRequestsWith503)
{
    sylar::IOManager acceptor;
    sylar::IOManagerPool workers(2);
    auto routes = std::make_shared<sylar::ServletDispatch>();
    std::atomic_bool entered{false};
    routes->addExact(
        "/slow",
        std::make_shared<sylar::FunctionServlet>(
            [&](const sylar::HttpRequest&)
            {
                entered.store(true);
                sylar::IOManager::GetThis()->sleepFor(50ms);
                sylar::HttpResponse response;
                response.body = "done";
                return response;
            }
        )
    );
    auto limiter = std::make_shared<sylar::RequestLimiter>(
        sylar::RequestLimiter::Options{
            .initial = 1,
            .minimum = 1,
            .maximum = 1,
            .window = 4,
        }
    );
    sylar::HttpServer server(acceptor, workers, routes, {}, limiter);
    server.start("127.0.0.1", 0);
    const auto uri = sylar::Uri::parse(
        "http://127.0.0.1:" + std::to_string(server.localPort()) +
        "/slow"
    );
    sylar::HttpResponse first;
    sylar::HttpResponse second;
    acceptor.schedule(
        [&] { first = sylar::HttpClient::request(acceptor, uri); }
    );
    acceptor.schedule(
        [&]
        {
            while (!entered.load())
            {
                acceptor.sleepFor(1ms);
            }
            second = sylar::HttpClient::request(acceptor, uri);
            server.stop();
        }
    );
    acceptor.run();
    workers.stop();
    EXPECT_EQ(first.status, 200);
    EXPECT_EQ(second.status, 503);
    EXPECT_EQ(limiter->snapshot().rejected, 1);
    EXPECT_EQ(limiter->snapshot().in_flight, 0);
}
