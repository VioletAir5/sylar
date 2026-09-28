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

TEST(SocketTest, ExchangesUdpDatagramAndReportsSource)
{
    sylar::IOManager io;
    auto receiver = sylar::Socket::udp();
    receiver.bind(sylar::Address::ipv4("127.0.0.1", 0));
    auto sender = sylar::Socket::udp();
    sender.bind(sylar::Address::ipv4("127.0.0.1", 0));
    std::string received;
    std::uint16_t source_port = 0;
    io.schedule(
        [&]
        {
            std::array<std::byte, 16> buffer{};
            auto datagram = receiver.recvFrom(buffer);
            if (!datagram)
            {
                EXPECT_EQ(
                    io.waitReadable(receiver.fd(), 1s),
                    sylar::IOManager::WaitResult::Ready
                );
                datagram = receiver.recvFrom(buffer);
            }
            ASSERT_TRUE(datagram);
            received.assign(
                reinterpret_cast<const char*>(buffer.data()),
                datagram->count
            );
            source_port = datagram->source.port();
        }
    );
    io.schedule(
        [&]
        {
            const std::string message = "udp";
            EXPECT_EQ(
                sender.sendTo(
                    std::as_bytes(std::span(message)),
                    receiver.localAddress()
                ),
                3
            );
        }
    );
    io.run();
    EXPECT_EQ(received, "udp");
    EXPECT_EQ(source_port, sender.localAddress().port());
}
