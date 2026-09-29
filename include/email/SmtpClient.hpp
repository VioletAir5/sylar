#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "net/TlsStream.hpp"

#include "fiber/IOManager.hpp"

namespace sylar
{

struct SmtpMessage
{
    std::string from;
    std::vector<std::string> recipients;
    // Complete RFC 5322 message, including headers and body.
    std::string data;
};

class SmtpClient
{
public:
    static void send(
        IOManager& io,
        const std::string& host,
        std::uint16_t port,
        const SmtpMessage& message,
        std::string helo_host = "localhost",
        std::chrono::milliseconds timeout = std::chrono::seconds(30),
        std::shared_ptr<TlsContext> tls = {},
        bool start_tls = false
    );
};

} // namespace sylar