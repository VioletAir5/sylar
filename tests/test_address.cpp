#include <gtest/gtest.h>

#include "net/Address.hpp"

TEST(AddressTest, ParsesFamiliesAndPorts)
{
    const auto v4 = sylar::Address::ipv4("127.0.0.1", 8080);
    EXPECT_EQ(v4.family(), AF_INET);
    EXPECT_EQ(v4.port(), 8080);
    EXPECT_EQ(v4.toString(), "127.0.0.1:8080");

    const auto v6 = sylar::Address::ipv6("::1", 443);
    EXPECT_EQ(v6.family(), AF_INET6);
    EXPECT_EQ(v6.port(), 443);
    EXPECT_EQ(v6.toString(), "[::1]:443");

    const auto local = sylar::Address::unix("/tmp/sylar-test.sock");
    EXPECT_EQ(local.family(), AF_UNIX);
    EXPECT_EQ(local.toString(), "/tmp/sylar-test.sock");
    EXPECT_THROW(local.port(), std::logic_error);
}

TEST(AddressTest, ResolvesNumericHost)
{
    const auto addresses = sylar::Address::resolve(
        "127.0.0.1",
        "9000",
        AF_INET
    );
    ASSERT_FALSE(addresses.empty());
    EXPECT_EQ(addresses.front().toString(), "127.0.0.1:9000");
}
