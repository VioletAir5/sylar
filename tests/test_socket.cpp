#include <array>
#include <chrono>
#include <cstddef>
#include <string>

#include <gtest/gtest.h>

#include "net/Address.hpp"
#include "net/Socket.hpp"

#include "fiber/IOManager.hpp"

using namespace std::chrono_literals;

TEST(SocketTest, ConnectAcceptAndExchangeOnFiber)
{
    sylar::IOManager io;
    auto listener = sylar::Socket::tcp();
    listener.setReuseAddress();
    listener.bind(sylar::Address::ipv4("127.0.0.1", 0));
    listener.listen();
    const auto port = listener.localAddress().port();
    std::string received;

    io.schedule(
        [&]
        {
            std::optional<sylar::Socket> client;
            while (!(client = listener.accept()))
            {
                ASSERT_EQ(
                    io.waitReadable(listener.fd()),
                    sylar::IOManager::WaitResult::Ready
                );
            }
            std::array<std::byte, 8> buffer{};
            ssize_t count = -1;
            while ((count = client->recvSome(buffer)) < 0)
            {
                ASSERT_EQ(
                    io.waitReadable(client->fd()),
                    sylar::IOManager::WaitResult::Ready
                );
            }
            received.assign(
                reinterpret_cast<const char*>(buffer.data()),
                count
            );
            ASSERT_EQ(
                client->sendSome(
                    std::span<const std::byte>(buffer.data(), count)
                ),
                count
            );
        }
    );
    io.schedule(
        [&]
        {
            auto client = sylar::Socket::tcp();
            client.connect(
                io,
                sylar::Address::ipv4("127.0.0.1", port),
                1s
            );
            EXPECT_EQ(client.peerAddress().port(), port);
            const std::string message = "hello";
            auto input = std::as_bytes(std::span(message));
            ASSERT_EQ(client.sendSome(input), 5);
            std::array<std::byte, 8> response{};
            ssize_t count = -1;
            while ((count = client.recvSome(response)) < 0)
            {
                ASSERT_EQ(
                    io.waitReadable(client.fd()),
                    sylar::IOManager::WaitResult::Ready
                );
            }
            EXPECT_EQ(count, 5);
            EXPECT_EQ(
                std::string(
                    reinterpret_cast<const char*>(response.data()),
                    count
                ),
                message
            );
        }
    );
    io.run();
    EXPECT_EQ(received, "hello");
}
