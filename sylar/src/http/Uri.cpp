#include "http/Uri.hpp"

#include <charconv>
#include <stdexcept>

namespace sylar
{

Uri Uri::parse(std::string_view text)
{
    Uri result;
    const auto scheme_end = text.find("://");
    if (scheme_end == std::string_view::npos)
    {
        throw std::invalid_argument("URL has no scheme");
    }
    result.scheme = text.substr(0, scheme_end);
    if (result.scheme != "http" && result.scheme != "https" &&
        result.scheme != "ws" && result.scheme != "wss")
    {
        throw std::invalid_argument("unsupported URL scheme");
    }
    text.remove_prefix(scheme_end + 3);
    const auto authority_end = text.find_first_of("/?#");
    const auto authority = text.substr(0, authority_end);
    if (authority.empty() ||
        authority.find('@') != std::string_view::npos)
    {
        throw std::invalid_argument("invalid URL authority");
    }

    std::string_view port_text;
    if (authority.front() == '[')
    {
        const auto close = authority.find(']');
        if (close == std::string_view::npos)
        {
            throw std::invalid_argument("invalid IPv6 URL host");
        }
        result.host = authority.substr(1, close - 1);
        if (close + 1 < authority.size())
        {
            if (authority[close + 1] != ':')
            {
                throw std::invalid_argument("invalid URL port");
            }
            port_text = authority.substr(close + 2);
            if (port_text.empty())
            {
                throw std::invalid_argument("invalid URL port");
            }
        }
    }
    else
    {
        const auto colon = authority.find(':');
        result.host = authority.substr(0, colon);
        if (colon != std::string_view::npos)
        {
            port_text = authority.substr(colon + 1);
            if (port_text.empty())
            {
                throw std::invalid_argument("invalid URL port");
            }
        }
    }
    if (result.host.empty() ||
        result.host.find_first_of(" \t\r\n") != std::string::npos)
    {
        throw std::invalid_argument("invalid URL host");
    }
    result.port = (result.scheme == "https" || result.scheme == "wss")
                      ? 443
                      : 80;
    if (!port_text.empty())
    {
        unsigned number = 0;
        const auto [end, error] = std::from_chars(
            port_text.data(),
            port_text.data() + port_text.size(),
            number
        );
        if (error != std::errc{} ||
            end != port_text.data() + port_text.size() ||
            number == 0 || number > 65535)
        {
            throw std::invalid_argument("invalid URL port");
        }
        result.port = static_cast<std::uint16_t>(number);
    }
    if (authority_end != std::string_view::npos)
    {
        text.remove_prefix(authority_end);
        const auto fragment = text.find('#');
        text = text.substr(0, fragment);
    }
    else
    {
        text = {};
    }
    if (text.empty())
    {
        result.target = "/";
    }
    else if (text.front() == '?')
    {
        result.target = "/" + std::string(text);
    }
    else if (text.front() == '/')
    {
        result.target = text;
    }
    else
    {
        throw std::invalid_argument("invalid URL target");
    }
    if (result.target.find_first_of(" \t\r\n") != std::string::npos)
    {
        throw std::invalid_argument("invalid URL target");
    }
    return result;
}

std::string Uri::authority() const
{
    std::string result = host.find(':') != std::string::npos
                             ? "[" + host + "]"
                             : host;
    if (((scheme == "http" || scheme == "ws") && port != 80) ||
        ((scheme == "https" || scheme == "wss") && port != 443))
    {
        result += ":" + std::to_string(port);
    }
    return result;
}

} // namespace sylar
