#include "net/Address.hpp"

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/un.h>

#include <cstddef>
#include <cstring>
#include <stdexcept>
#include <system_error>

namespace sylar
{

Address::Address(const sockaddr* address, socklen_t length)
    : m_length(length)
{
    if (!address || length == 0 || length > sizeof(m_storage))
    {
        throw std::invalid_argument("invalid socket address");
    }
    std::memcpy(&m_storage, address, length);
}

std::vector<Address> Address::resolve(
    const std::string& host,
    const std::string& service,
    int family,
    int type
)
{
    addrinfo hints{};
    hints.ai_family = family;
    hints.ai_socktype = type;
    addrinfo* results = nullptr;
    const int error = ::getaddrinfo(
        host.c_str(),
        service.c_str(),
        &hints,
        &results
    );
    if (error != 0)
    {
        throw std::runtime_error(::gai_strerror(error));
    }
    std::vector<Address> addresses;
    try
    {
        for (auto* item = results; item; item = item->ai_next)
        {
            addresses.emplace_back(
                item->ai_addr,
                static_cast<socklen_t>(item->ai_addrlen)
            );
        }
    }
    catch (...)
    {
        ::freeaddrinfo(results);
        throw;
    }
    ::freeaddrinfo(results);
    return addresses;
}

Address Address::ipv4(const std::string& host, std::uint16_t port)
{
    sockaddr_in value{};
    value.sin_family = AF_INET;
    value.sin_port = htons(port);
    if (::inet_pton(AF_INET, host.c_str(), &value.sin_addr) != 1)
    {
        throw std::invalid_argument("invalid IPv4 address");
    }
    return Address(
        reinterpret_cast<const sockaddr*>(&value),
        sizeof(value)
    );
}

Address Address::ipv6(const std::string& host, std::uint16_t port)
{
    sockaddr_in6 value{};
    value.sin6_family = AF_INET6;
    value.sin6_port = htons(port);
    if (::inet_pton(AF_INET6, host.c_str(), &value.sin6_addr) != 1)
    {
        throw std::invalid_argument("invalid IPv6 address");
    }
    return Address(
        reinterpret_cast<const sockaddr*>(&value),
        sizeof(value)
    );
}

Address Address::unix(std::string_view path)
{
    sockaddr_un value{};
    if (path.empty() || path.size() >= sizeof(value.sun_path))
    {
        throw std::invalid_argument("invalid Unix socket path");
    }
    value.sun_family = AF_UNIX;
    std::memcpy(value.sun_path, path.data(), path.size());
    const auto length = static_cast<socklen_t>(
        offsetof(sockaddr_un, sun_path) + path.size() + 1
    );
    return Address(reinterpret_cast<const sockaddr*>(&value), length);
}

const sockaddr* Address::data() const noexcept
{
    return reinterpret_cast<const sockaddr*>(&m_storage);
}

sockaddr* Address::data() noexcept
{
    return reinterpret_cast<sockaddr*>(&m_storage);
}

socklen_t Address::size() const noexcept
{
    return m_length;
}

int Address::family() const noexcept
{
    return m_storage.ss_family;
}

std::uint16_t Address::port() const
{
    if (family() == AF_INET)
    {
        const auto* value = reinterpret_cast<const sockaddr_in*>(
            &m_storage
        );
        return ntohs(value->sin_port);
    }
    if (family() == AF_INET6)
    {
        const auto* value = reinterpret_cast<const sockaddr_in6*>(
            &m_storage
        );
        return ntohs(value->sin6_port);
    }
    throw std::logic_error("Unix address has no port");
}

std::string Address::toString() const
{
    char buffer[INET6_ADDRSTRLEN]{};
    if (family() == AF_INET)
    {
        const auto* value = reinterpret_cast<const sockaddr_in*>(
            &m_storage
        );
        if (!::inet_ntop(
                AF_INET,
                &value->sin_addr,
                buffer,
                sizeof(buffer)
            ))
        {
            throw std::system_error(
                errno,
                std::generic_category(),
                "inet_ntop"
            );
        }
        return std::string(buffer) + ":" + std::to_string(port());
    }
    if (family() == AF_INET6)
    {
        const auto* value = reinterpret_cast<const sockaddr_in6*>(
            &m_storage
        );
        if (!::inet_ntop(
                AF_INET6,
                &value->sin6_addr,
                buffer,
                sizeof(buffer)
            ))
        {
            throw std::system_error(
                errno,
                std::generic_category(),
                "inet_ntop"
            );
        }
        return "[" + std::string(buffer) +
               "]:" + std::to_string(port());
    }
    if (family() == AF_UNIX)
    {
        const auto* value = reinterpret_cast<const sockaddr_un*>(
            &m_storage
        );
        return std::string(
            value->sun_path,
            strnlen(value->sun_path, sizeof(value->sun_path))
        );
    }
    throw std::logic_error("unsupported address family");
}

} // namespace sylar
