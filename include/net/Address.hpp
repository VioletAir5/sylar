#pragma once

#include <sys/socket.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace sylar
{

class Address
{
public:
    Address(const sockaddr* address, socklen_t length);

    static std::vector<Address> resolve(
        const std::string& host,
        const std::string& service,
        int family = AF_UNSPEC,
        int type = SOCK_STREAM
    );
    static Address ipv4(const std::string& host, std::uint16_t port);
    static Address ipv6(const std::string& host, std::uint16_t port);
    static Address unix(std::string_view path);

    const sockaddr* data() const noexcept;
    sockaddr* data() noexcept;
    socklen_t size() const noexcept;
    int family() const noexcept;
    std::uint16_t port() const;
    std::string toString() const;

private:
    sockaddr_storage m_storage{};
    socklen_t m_length = 0;
};

} // namespace sylar
