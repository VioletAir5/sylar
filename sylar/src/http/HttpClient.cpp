#include "http/HttpClient.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cstddef>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <utility>

#include "net/Address.hpp"
#include "net/Socket.hpp"
#include "net/SocketStream.hpp"
#include "net/TlsStream.hpp"

namespace sylar
{
namespace
{

std::string lower(std::string_view value)
{
    std::string result(value);
    std::transform(
        result.begin(),
        result.end(),
        result.begin(),
        [](unsigned char ch)
        { return static_cast<char>(std::tolower(ch)); }
    );
    return result;
}

class Reader
{
public:
    explicit Reader(Stream& stream)
        : m_stream(stream)
    {
    }

    std::string line(std::size_t limit = 16 * 1024)
    {
        for (;;)
        {
            if (const auto end = m_buffer.find("\r\n");
                end != std::string::npos)
            {
                if (end > limit)
                {
                    throw std::invalid_argument("HTTP line too large"
                    );
                }
                auto result = m_buffer.substr(0, end);
                m_buffer.erase(0, end + 2);
                return result;
            }
            if (m_buffer.size() > limit)
            {
                throw std::invalid_argument("HTTP line too large");
            }
            fill();
        }
    }

    std::string take(std::size_t count)
    {
        while (m_buffer.size() < count)
        {
            fill();
        }
        auto result = m_buffer.substr(0, count);
        m_buffer.erase(0, count);
        return result;
    }

    std::string rest(std::size_t limit)
    {
        std::string result = std::move(m_buffer);
        m_buffer.clear();
        std::array<std::byte, 4096> chunk{};
        while (true)
        {
            if (result.size() > limit)
            {
                throw std::invalid_argument("HTTP body too large");
            }
            const auto count = m_stream.readSome(chunk);
            if (count == 0)
            {
                return result;
            }
            result.append(
                reinterpret_cast<const char*>(chunk.data()),
                count
            );
        }
    }

private:
    void fill()
    {
        std::array<std::byte, 4096> chunk{};
        const auto count = m_stream.readSome(chunk);
        if (count == 0)
        {
            throw std::invalid_argument("unexpected HTTP response EOF"
            );
        }
        m_buffer.append(
            reinterpret_cast<const char*>(chunk.data()),
            count
        );
    }

    Stream& m_stream;
    std::string m_buffer;
};

HttpResponse readResponse(
    Stream& stream,
    std::string_view method,
    std::size_t max_body = 16 * 1024 * 1024
)
{
    Reader reader(stream);
    int informational = 0;
    for (;;)
    {
        auto status_line = reader.line();
        const auto first = status_line.find(' ');
        const auto second = status_line.find(' ', first + 1);
        if (first == std::string::npos ||
            second == std::string::npos ||
            (status_line.substr(0, first) != "HTTP/1.1" &&
             status_line.substr(0, first) != "HTTP/1.0"))
        {
            throw std::invalid_argument("invalid HTTP response line");
        }
        HttpResponse response;
        const auto status_text = std::string_view(status_line)
                                     .substr(
                                         first + 1,
                                         second - first - 1
                                     );
        const auto [end, error] = std::from_chars(
            status_text.data(),
            status_text.data() + status_text.size(),
            response.status
        );
        if (error != std::errc{} ||
            end != status_text.data() + status_text.size() ||
            response.status < 100 || response.status > 999)
        {
            throw std::invalid_argument("invalid HTTP status");
        }
        response.reason = status_line.substr(second + 1);
        std::size_t header_bytes = status_line.size() + 2;
        while (true)
        {
            auto line = reader.line();
            header_bytes += line.size() + 2;
            if (header_bytes > 16 * 1024)
            {
                throw std::invalid_argument("HTTP headers too large");
            }
            if (line.empty())
            {
                break;
            }
            const auto colon = line.find(':');
            if (colon == std::string::npos || colon == 0)
            {
                throw std::invalid_argument(
                    "invalid HTTP response header"
                );
            }
            const auto name = lower(
                std::string_view(line).substr(0, colon)
            );
            const auto start = line.find_first_not_of(
                " \t",
                colon + 1
            );
            const auto value = start == std::string::npos
                                   ? ""
                                   : line.substr(start);
            if (response.headers.contains(name))
            {
                throw std::invalid_argument(
                    "duplicate HTTP response header"
                );
            }
            response.headers.emplace(name, value);
        }
        if (response.status >= 100 && response.status < 200)
        {
            if (response.status == 101 || ++informational > 8)
            {
                throw std::invalid_argument(
                    "unsupported HTTP informational response"
                );
            }
            continue;
        }
        if (method == "HEAD" || response.status == 204 ||
            response.status == 304)
        {
            return response;
        }
        const auto transfer = response.headers.find(
            "transfer-encoding"
        );
        const auto length = response.headers.find("content-length");
        if (transfer != response.headers.end())
        {
            if (length != response.headers.end() ||
                lower(transfer->second) != "chunked")
            {
                throw std::invalid_argument(
                    "unsupported HTTP transfer encoding"
                );
            }
            while (true)
            {
                const auto line = reader.line();
                const auto semicolon = line.find(';');
                const auto hex = std::string_view(line).substr(
                    0,
                    semicolon
                );
                std::size_t count = 0;
                const auto [last, parse_error] = std::from_chars(
                    hex.data(),
                    hex.data() + hex.size(),
                    count,
                    16
                );
                if (parse_error != std::errc{} ||
                    last != hex.data() + hex.size() ||
                    count > max_body - response.body.size())
                {
                    throw std::invalid_argument(
                        "invalid HTTP chunk size"
                    );
                }
                if (count == 0)
                {
                    while (!reader.line().empty())
                    {
                    }
                    break;
                }
                response.body += reader.take(count);
                if (reader.take(2) != "\r\n")
                {
                    throw std::invalid_argument(
                        "invalid HTTP chunk terminator"
                    );
                }
            }
        }
        else if (length != response.headers.end())
        {
            std::size_t count = 0;
            const auto [last, parse_error] = std::from_chars(
                length->second.data(),
                length->second.data() + length->second.size(),
                count
            );
            if (parse_error != std::errc{} ||
                last !=
                    length->second.data() + length->second.size() ||
                count > max_body)
            {
                throw std::invalid_argument(
                    "invalid HTTP Content-Length"
                );
            }
            response.body = reader.take(count);
        }
        else
        {
            response.body = reader.rest(max_body);
        }
        return response;
    }
}

} // namespace

HttpResponse HttpClient::request(
    IOManager& io,
    const Uri& uri,
    std::string method,
    std::string body,
    std::unordered_map<std::string, std::string> headers,
    std::chrono::milliseconds timeout,
    std::shared_ptr<TlsContext> tls
)
{
    if (uri.scheme != "http" && uri.scheme != "https")
    {
        throw std::invalid_argument("unsupported HTTP URL scheme");
    }
    if (method.empty() ||
        method.find_first_of(" \t\r\n") != std::string::npos)
    {
        throw std::invalid_argument("invalid HTTP method");
    }
    const auto addresses = Address::resolve(
        uri.host,
        std::to_string(uri.port)
    );
    if (addresses.empty())
    {
        throw std::runtime_error("HTTP host has no addresses");
    }
    auto socket = Socket::tcp(addresses.front().family());
    socket.connect(io, addresses.front(), timeout);
    std::unique_ptr<Stream> stream;
    if (uri.scheme == "https")
    {
        if (!tls)
        {
            tls = TlsContext::client();
        }
        stream = std::make_unique<TlsStream>(
            io,
            std::move(socket),
            std::move(tls),
            TlsStream::Role::Client,
            uri.host,
            timeout
        );
    }
    else
    {
        stream = std::make_unique<SocketStream>(
            io,
            std::move(socket),
            timeout
        );
    }
    std::string wire = method + " " + uri.target +
                       " HTTP/1.1\r\nHost: " + uri.authority() +
                       "\r\n";
    for (const auto& [name, value] : headers)
    {
        if (name.empty() ||
            name.find_first_of(":\r\n") != std::string::npos ||
            value.find_first_of("\r\n") != std::string::npos)
        {
            throw std::invalid_argument("invalid HTTP request header"
            );
        }
        const auto normalized = lower(name);
        if (normalized == "host" || normalized == "content-length" ||
            normalized == "connection" ||
            normalized == "transfer-encoding")
        {
            continue;
        }
        wire += name + ": " + value + "\r\n";
    }
    wire += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    wire += "Connection: close\r\n\r\n";
    wire += body;
    stream->writeAll(std::as_bytes(std::span(wire)));
    return readResponse(*stream, method);
}

} // namespace sylar
