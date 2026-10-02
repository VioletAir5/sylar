#include <atomic>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

#include "fiber/BlockingExecutor.hpp"
#include "fiber/IOManager.hpp"
#include "http/WebSocketServer.hpp"
#include "redis/RedisClient.hpp"
#include "redis/RedisStreamBus.hpp"

namespace
{

std::uint16_t portFrom(const char* value)
{
    unsigned port = 0;
    std::string_view text(value);
    const auto [end, error] = std::from_chars(
        text.data(),
        text.data() + text.size(),
        port
    );
    if (error != std::errc{} || end != text.data() + text.size() ||
        port == 0 || port > 65535)
    {
        throw std::invalid_argument("invalid port");
    }
    return static_cast<std::uint16_t>(port);
}

} // namespace

int main(int argc, char** argv)
{
    try
    {
        if (argc != 3)
        {
            throw std::invalid_argument(
                "usage: redis_ws_server LISTEN_PORT REDIS_PORT"
            );
        }
        const auto listen_port = portFrom(argv[1]);
        const auto redis_port = portFrom(argv[2]);
        sylar::IOManager io;
        // Eight blocking XREAD subscribers plus one reserved
        // publisher lane.
        sylar::BlockingExecutor blocking(9);
        unsigned retention = 1000;
        if (const auto* value = std::getenv("SYLAR_STREAM_RETENTION"))
        {
            retention = portFrom(value);
        }
        sylar::RedisStreamBus bus("sylar:room", retention);
        std::atomic_uint32_t subscribers{0};
        sylar::WebSocketServer server(
            io,
            [&](sylar::WebSocketSession& session,
                const sylar::WebSocketMessage& message)
            {
                if (message.type !=
                    sylar::WebSocketMessage::Type::Text)
                {
                    session.sendText("ERROR text-only");
                    return;
                }
                const auto& command = message.payload;
                if (command.starts_with("PUB "))
                {
                    try
                    {
                        const auto id = blocking.await(
                            io,
                            [&]
                            {
                                auto redis = sylar::RedisClient::
                                    connect("127.0.0.1", redis_port);
                                return bus.publish(
                                    redis,
                                    std::string_view(command).substr(4
                                    )
                                );
                            }
                        );
                        session.sendText("ACK " + id);
                    }
                    catch (const std::exception&)
                    {
                        session.sendText("ERROR redis-unavailable");
                    }
                    return;
                }
                if (!command.starts_with("SUB ") ||
                    !sylar::RedisStreamBus::validId(
                        std::string_view(command).substr(4)
                    ))
                {
                    session.sendText("ERROR expected-PUB-or-SUB");
                    return;
                }
                if (subscribers.fetch_add(1) >= 8)
                {
                    subscribers.fetch_sub(1);
                    session.sendText("ERROR subscriber-capacity");
                    return;
                }
                struct SubscriberGuard
                {
                    std::atomic_uint32_t& count;
                    ~SubscriberGuard()
                    {
                        count.fetch_sub(1);
                    }
                } guard{subscribers};
                auto last_id = command.substr(4);
                try
                {
                    auto redis = blocking.await(
                        io,
                        [&] {
                            return sylar::RedisClient::connect(
                                "127.0.0.1",
                                redis_port
                            );
                        }
                    );
                    const auto
                        expires = std::chrono::steady_clock::now() +
                                  std::chrono::seconds(30);
                    while (std::chrono::steady_clock::now() < expires)
                    {
                        const auto events = blocking.await(
                            io,
                            [&]
                            { return bus.readAfter(redis, last_id); }
                        );
                        for (const auto& event : events)
                        {
                            session.sendText(
                                "EVENT " + event.id + " " +
                                event.payload
                            );
                            last_id = event.id;
                        }
                    }
                    session.close(1000);
                }
                catch (const sylar::ReplayGap&)
                {
                    session.sendText("GAP replay-window-expired");
                    session.close(1008);
                }
                catch (const std::exception&)
                {
                    // Redis or socket failure closes the subscriber.
                }
            }
        );
        server.start(sylar::Address::ipv4("127.0.0.1", listen_port));
        std::cout << "redis_ws_server listening on port "
                  << server.localPort() << std::endl;
        io.runForever();
    }
    catch (const std::exception& error)
    {
        std::cerr << "redis_ws_server: " << error.what() << std::endl;
        return 1;
    }
}
