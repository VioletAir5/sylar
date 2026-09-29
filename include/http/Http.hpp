#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

#include "net/Stream.hpp"

namespace sylar
{

struct HttpRequest
{
    std::string method;
    std::string target;
    std::string version;
    std::unordered_map<std::string, std::string> headers;
    std::string body;

    std::optional<std::string_view> header(std::string_view name
    ) const;
    bool keepAlive() const;
};

struct HttpResponse
{
    int status = 200;
    std::string reason = "OK";
    std::unordered_map<std::string, std::string> headers;
    std::string body;

    std::string
    serialize(bool keep_alive = true, bool head_only = false) const;
};

class HttpSession
{
public:
    explicit HttpSession(
        Stream& stream,
        std::size_t max_header = 16 * 1024,
        std::size_t max_body = 1024 * 1024
    );

    // false means a clean EOF before the next request.
    bool readRequest(HttpRequest& request);
    void sendResponse(
        const HttpResponse& response,
        bool keep_alive,
        bool head_only = false
    );
    std::string takeBuffered();

private:
    Stream& m_stream;
    std::string m_buffer;
    std::size_t m_maxHeader;
    std::size_t m_maxBody;
};

} // namespace sylar
