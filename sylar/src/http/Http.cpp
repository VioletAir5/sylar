#include "http/Http.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <stdexcept>
#include <utility>

namespace sylar
{
namespace
{

std::string lower(std::string_view text)
{
    std::string result(text);
    std::transform(
        result.begin(),
        result.end(),
        result.begin(),
        [](unsigned char c)
        {
            return static_cast<char>(
                c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c
            );
        }
    );
    return result;
}

std::string_view trim(std::string_view text)
{
    const auto first = text.find_first_not_of(" \t");
    if (first == std::string_view::npos)
    {
        return {};
    }
    const auto last = text.find_last_not_of(" \t");
    return text.substr(first, last - first + 1);
}

bool hasToken(std::string_view value, std::string_view expected)
{
    while (!value.empty())
    {
        const auto comma = value.find(',');
        const auto token = trim(value.substr(0, comma));
        if (lower(token) == expected)
        {
            return true;
        }
        if (comma == std::string_view::npos)
        {
            break;
        }
        value.remove_prefix(comma + 1);
    }
    return false;
}

bool validToken(std::string_view text)
{
    if (text.empty())
    {
        return false;
    }
    for (unsigned char ch : text)
    {
        const bool letter = (ch >= 'A' && ch <= 'Z') ||
                            (ch >= 'a' && ch <= 'z');
        const bool digit = ch >= '0' && ch <= '9';
        if (!letter && !digit &&
            std::string_view("!#$%&'*+-.^_`|~").find(ch) ==
                std::string_view::npos)
        {
            return false;
        }
    }
    return true;
}

bool validFieldValue(std::string_view text)
{
    return std::all_of(
        text.begin(),
        text.end(),
        [](unsigned char ch)
        { return ch == '\t' || (ch >= ' ' && ch != 0x7f); }
    );
}

void parseHead(std::string_view head, HttpRequest& request)
{
    const auto line_end = head.find("\r\n");
    if (line_end == std::string_view::npos)
    {
        throw std::invalid_argument("missing request line");
    }
    const auto line = head.substr(0, line_end);
    const auto first = line.find(' ');
    const auto second = first == std::string_view::npos
                            ? first
                            : line.find(' ', first + 1);
    if (first == std::string_view::npos ||
        second == std::string_view::npos ||
        line.find(' ', second + 1) != std::string_view::npos)
    {
        throw std::invalid_argument("invalid request line");
    }
    request.method = line.substr(0, first);
    request.target = line.substr(first + 1, second - first - 1);
    request.version = line.substr(second + 1);
    if (!validToken(request.method) || request.target.empty() ||
        request.target[0] != '/' ||
        std::any_of(
            request.target.begin(),
            request.target.end(),
            [](unsigned char ch) { return ch <= ' ' || ch >= 0x7f; }
        ) ||
        (request.version != "HTTP/1.1" &&
         request.version != "HTTP/1.0"))
    {
        throw std::invalid_argument("unsupported HTTP request");
    }

    auto offset = line_end + 2;
    while (offset < head.size())
    {
        const auto end = head.find("\r\n", offset);
        if (end == std::string_view::npos)
        {
            throw std::invalid_argument("invalid HTTP headers");
        }
        const auto header = head.substr(offset, end - offset);
        const auto colon = header.find(':');
        if (colon == std::string_view::npos ||
            !validToken(header.substr(0, colon)))
        {
            throw std::invalid_argument("invalid HTTP header");
        }
        const auto name = lower(header.substr(0, colon));
        const auto value = trim(header.substr(colon + 1));
        if (!validFieldValue(value) || request.headers.contains(name))
        {
            throw std::invalid_argument(
                "invalid or duplicate HTTP header"
            );
        }
        request.headers.emplace(name, value);
        offset = end + 2;
    }
    if (request.version == "HTTP/1.1" &&
        (!request.headers.contains("host") ||
         request.headers.at("host").empty()))
    {
        throw std::invalid_argument("HTTP/1.1 requires Host");
    }
    if (const auto encoding = request.header("transfer-encoding"))
    {
        if (lower(*encoding) != "chunked" ||
            request.headers.contains("content-length"))
        {
            throw std::invalid_argument(
                "invalid HTTP transfer encoding"
            );
        }
    }
}

std::size_t
contentLength(const HttpRequest& request, std::size_t limit)
{
    const auto value = request.header("content-length");
    if (!value)
    {
        return 0;
    }
    std::size_t result = 0;
    const auto [end, error] = std::from_chars(
        value->data(),
        value->data() + value->size(),
        result
    );
    if (error != std::errc{} ||
        end != value->data() + value->size() || result > limit)
    {
        throw std::invalid_argument("invalid Content-Length");
    }
    return result;
}

} // namespace

std::optional<std::string_view>
HttpRequest::header(std::string_view name) const
{
    const auto found = headers.find(lower(name));
    if (found == headers.end())
    {
        return std::nullopt;
    }
    return found->second;
}

bool HttpRequest::keepAlive() const
{
    const auto value = header("connection");
    if (value && hasToken(*value, "close"))
    {
        return false;
    }
    return version == "HTTP/1.1" ||
           (value && hasToken(*value, "keep-alive"));
}

std::string
HttpResponse::serialize(bool keep_alive, bool head_only) const
{
    if (status < 100 || status > 999 || reason.empty() ||
        !validFieldValue(reason))
    {
        throw std::invalid_argument("invalid HTTP status");
    }
    std::string result = "HTTP/1.1 " + std::to_string(status) + " " +
                         reason + "\r\n";
    for (const auto& [name, value] : headers)
    {
        if (!validToken(name) || !validFieldValue(value))
        {
            throw std::invalid_argument("invalid response header");
        }
        const auto normalized = lower(name);
        if (normalized == "content-length" ||
            normalized == "connection" ||
            normalized == "transfer-encoding")
        {
            continue;
        }
        result += name + ": " + value + "\r\n";
    }
    const bool body_allowed = status >= 200 && status != 204 &&
                              status != 304;
    if (body_allowed)
    {
        result += "Content-Length: " + std::to_string(body.size()) +
                  "\r\n";
    }
    result += keep_alive ? "Connection: keep-alive\r\n"
                         : "Connection: close\r\n";
    result += "\r\n";
    if (body_allowed && !head_only)
    {
        result += body;
    }
    return result;
}

HttpSession::HttpSession(
    Stream& stream, std::size_t max_header, std::size_t max_body
)
    : m_stream(stream)
    , m_maxHeader(max_header)
    , m_maxBody(max_body)
{
    if (max_header == 0)
    {
        throw std::invalid_argument("zero HTTP header limit");
    }
}

bool HttpSession::readRequest(HttpRequest& request)
{
    std::array<std::byte, 4096> chunk{};
    auto boundary = m_buffer.find("\r\n\r\n");
    while (boundary == std::string::npos)
    {
        if (m_buffer.size() > m_maxHeader)
        {
            throw std::invalid_argument("HTTP header too large");
        }
        const auto count = m_stream.readSome(chunk);
        if (count == 0)
        {
            if (m_buffer.empty())
            {
                return false;
            }
            throw std::invalid_argument(
                "unexpected EOF in HTTP header"
            );
        }
        m_buffer.append(
            reinterpret_cast<const char*>(chunk.data()),
            count
        );
        boundary = m_buffer.find("\r\n\r\n");
    }
    if (boundary + 4 > m_maxHeader)
    {
        throw std::invalid_argument("HTTP header too large");
    }

    HttpRequest parsed;
    parseHead(
        std::string_view(m_buffer).substr(0, boundary + 2),
        parsed
    );
    auto readMore = [&]
    {
        const auto count = m_stream.readSome(chunk);
        if (count == 0)
        {
            throw std::invalid_argument("unexpected EOF in HTTP body"
            );
        }
        m_buffer.append(
            reinterpret_cast<const char*>(chunk.data()),
            count
        );
    };
    if (parsed.header("transfer-encoding"))
    {
        std::size_t position = boundary + 4;
        std::size_t trailer_bytes = 0;
        for (;;)
        {
            auto line_end = m_buffer.find("\r\n", position);
            while (line_end == std::string::npos)
            {
                if (m_buffer.size() - position > m_maxHeader)
                {
                    throw std::invalid_argument(
                        "HTTP chunk line too large"
                    );
                }
                readMore();
                line_end = m_buffer.find("\r\n", position);
            }
            const auto line = std::string_view(m_buffer).substr(
                position,
                line_end - position
            );
            const auto semicolon = line.find(';');
            const auto hex = line.substr(0, semicolon);
            std::size_t count = 0;
            const auto [last, error] = std::from_chars(
                hex.data(),
                hex.data() + hex.size(),
                count,
                16
            );
            if (hex.empty() || error != std::errc{} ||
                last != hex.data() + hex.size() ||
                count > m_maxBody - parsed.body.size())
            {
                throw std::invalid_argument("invalid HTTP chunk size"
                );
            }
            position = line_end + 2;
            if (count == 0)
            {
                for (;;)
                {
                    line_end = m_buffer.find("\r\n", position);
                    while (line_end == std::string::npos)
                    {
                        if (m_buffer.size() - position +
                                trailer_bytes >
                            m_maxHeader)
                        {
                            throw std::invalid_argument(
                                "HTTP trailers too large"
                            );
                        }
                        readMore();
                        line_end = m_buffer.find("\r\n", position);
                    }
                    const auto line_size = line_end - position;
                    trailer_bytes += line_size + 2;
                    if (trailer_bytes > m_maxHeader)
                    {
                        throw std::invalid_argument(
                            "HTTP trailers too large"
                        );
                    }
                    if (line_size != 0)
                    {
                        const auto trailer = std::string_view(m_buffer
                        )
                                                 .substr(
                                                     position,
                                                     line_size
                                                 );
                        const auto colon = trailer.find(':');
                        if (colon == std::string_view::npos ||
                            !validToken(trailer.substr(0, colon)))
                        {
                            throw std::invalid_argument(
                                "invalid HTTP trailer"
                            );
                        }
                    }
                    position = line_end + 2;
                    if (line_size == 0)
                    {
                        break;
                    }
                }
                break;
            }
            while (m_buffer.size() - position < count + 2)
            {
                readMore();
            }
            parsed.body.append(m_buffer, position, count);
            position += count;
            if (m_buffer.compare(position, 2, "\r\n") != 0)
            {
                throw std::invalid_argument(
                    "invalid HTTP chunk terminator"
                );
            }
            position += 2;
        }
        m_buffer.erase(0, position);
    }
    else
    {
        const auto body_size = contentLength(parsed, m_maxBody);
        const auto total = boundary + 4 + body_size;
        while (m_buffer.size() < total)
        {
            readMore();
        }
        parsed.body = m_buffer.substr(boundary + 4, body_size);
        m_buffer.erase(0, total);
    }
    request = std::move(parsed);
    return true;
}

std::string HttpSession::takeBuffered()
{
    return std::move(m_buffer);
}

void HttpSession::sendResponse(
    const HttpResponse& response, bool keep_alive, bool head_only
)
{
    const auto data = response.serialize(keep_alive, head_only);
    m_stream.writeAll(std::as_bytes(std::span(data)));
}

} // namespace sylar
