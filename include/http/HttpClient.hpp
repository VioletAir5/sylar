#pragma once

#include <chrono>
#include <memory>
#include <string>
#include <unordered_map>

#include "net/TlsStream.hpp"

#include "fiber/IOManager.hpp"
#include "http/Http.hpp"
#include "http/Uri.hpp"

namespace sylar
{

class HttpClient
{
public:
    static HttpResponse request(
        IOManager& io,
        const Uri& uri,
        std::string method = "GET",
        std::string body = {},
        std::unordered_map<std::string, std::string> headers = {},
        std::chrono::milliseconds timeout = std::chrono::seconds(30),
        std::shared_ptr<TlsContext> tls = {}
    );
};

} // namespace sylar
