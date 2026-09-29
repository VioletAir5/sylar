#include <chrono>
#include <cstddef>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>

#include <gtest/gtest.h>

#include "net/Address.hpp"
#include "net/Socket.hpp"
#include "net/SocketStream.hpp"
#include "net/TcpServer.hpp"

#include "fiber/IOManager.hpp"
#include "http/Uri.hpp"
#include "http/WebSocketClient.hpp"
#include "http/WebSocketServer.hpp"

using namespace std::chrono_literals;

TEST(WebSocketClientTest, ExchangesMessagesAndCloses)
{
    sylar::IOManager io;
    sylar::WebSocketServer server(
        io,
        [](sylar::WebSocketSession& session,
           const sylar::WebSocketMessage& message)
        {
            if (message.type == sylar::WebSocketMessage::Type::Text)
            {
                session.sendText(message.payload);
            }
            else
            {
                session.sendBinary(message.payload);
            }
        }
    );
    server.start(sylar::Address::ipv4("127.0.0.1", 0));
    std::string text;
    std::string binary;
    bool closed = false;
    io.schedule(
        [&]
        {
            auto uri = sylar::Uri::parse(
                "ws://127.0.0.1:" +
                std::to_string(server.localPort()) + "/ws"
            );
            auto client = sylar::WebSocketClient::connect(
                io,
                uri,
                5s
            );
            client->sendText("hello");
            auto reply = client->readMessage();
            if (!reply ||
                reply->type != sylar::WebSocketMessage::Type::Text)
            {
                throw std::runtime_error(
                    "missing WebSocket text reply"
                );
            }
            text = reply->payload;
            const std::string payload(66000, '\x7f');
            client->sendBinary(payload);
            reply = client->readMessage();
            if (!reply ||
                reply->type != sylar::WebSocketMessage::Type::Binary)
            {
                throw std::runtime_error(
                    "missing WebSocket binary reply"
                );
            }
            binary = std::move(reply->payload);
            client->close();
            closed = !client->readMessage();
            server.stop();
        }
    );
    io.run();
    EXPECT_EQ(text, "hello");
    EXPECT_EQ(binary, std::string(66000, '\x7f'));
    EXPECT_TRUE(closed);
}

TEST(WebSocketClientTest, RejectsWrongAcceptKey)
{
    sylar::IOManager io;
    sylar::TcpServer server(
        io,
        [](sylar::IOManager& manager, int fd)
        {
            sylar::SocketStream stream(
                manager,
                sylar::Socket::borrow(fd),
                5s
            );
            std::byte bytes[1024];
            stream.readSome(bytes);
            const std::string
                reply = "HTTP/1.1 101 Switching Protocols\r\n"
                        "Upgrade: websocket\r\n"
                        "Connection: Upgrade\r\n"
                        "Sec-WebSocket-Accept: wrong\r\n\r\n";
            stream.writeAll(std::as_bytes(std::span(reply)));
        }
    );
    server.start("127.0.0.1", 0);
    bool rejected = false;
    io.schedule(
        [&]
        {
            const auto uri = sylar::Uri::parse(
                "ws://127.0.0.1:" +
                std::to_string(server.localPort()) + "/ws"
            );
            try
            {
                auto client = sylar::WebSocketClient::connect(
                    io,
                    uri,
                    5s
                );
            }
            catch (const std::invalid_argument&)
            {
                rejected = true;
            }
            server.stop();
        }
    );
    io.run();
    EXPECT_TRUE(rejected);
}

TEST(WebSocketClientTest, ParsesWebSocketUris)
{
    const auto ws = sylar::Uri::parse("ws://example.test/ws");
    EXPECT_EQ(ws.port, 80);
    EXPECT_EQ(ws.authority(), "example.test");
    const auto wss = sylar::Uri::parse("wss://example.test/ws");
    EXPECT_EQ(wss.port, 443);
    EXPECT_EQ(wss.authority(), "example.test");
    EXPECT_THROW(
        sylar::Uri::parse("ws://example.test:70000/"),
        std::invalid_argument
    );
}
