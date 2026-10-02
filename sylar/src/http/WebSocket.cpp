#include "http/WebSocket.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <limits>
#include <memory>
#include <span>
#include <stdexcept>
#include <system_error>
#include <utility>

#include <openssl/evp.h>

#include "fiber/RequestContext.hpp"

namespace sylar
{
namespace
{

constexpr char kCrLf[] = {13, 10, 0};
constexpr std::string_view
    kGuid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

std::string lower(std::string_view input)
{
    std::string result(input);
    std::transform(
        result.begin(),
        result.end(),
        result.begin(),
        [](unsigned char ch)
        { return static_cast<char>(std::tolower(ch)); }
    );
    return result;
}

bool hasToken(std::string_view value, std::string_view expected)
{
    while (!value.empty())
    {
        const auto comma = value.find(',');
        auto token = value.substr(0, comma);
        const auto first = token.find_first_not_of(" \t");
        if (first != std::string_view::npos)
        {
            const auto last = token.find_last_not_of(" \t");
            if (lower(token.substr(first, last - first + 1)) ==
                expected)
            {
                return true;
            }
        }
        if (comma == std::string_view::npos)
        {
            break;
        }
        value.remove_prefix(comma + 1);
    }
    return false;
}

bool validKey(std::string_view key)
{
    if (key.size() != 24 || key.substr(22) != "==")
    {
        return false;
    }
    std::array<unsigned char, 18> decoded{};
    return ::EVP_DecodeBlock(
               decoded.data(),
               reinterpret_cast<const unsigned char*>(key.data()),
               static_cast<int>(key.size())
           ) == 18;
}

bool validUtf8(std::string_view text)
{
    for (std::size_t i = 0; i < text.size();)
    {
        const auto first = static_cast<unsigned char>(text[i++]);
        if (first < 0x80)
        {
            continue;
        }
        int rest = 0;
        std::uint32_t value = 0;
        std::uint32_t minimum = 0;
        if ((first & 0xe0) == 0xc0)
        {
            rest = 1;
            value = first & 0x1f;
            minimum = 0x80;
        }
        else if ((first & 0xf0) == 0xe0)
        {
            rest = 2;
            value = first & 0x0f;
            minimum = 0x800;
        }
        else if ((first & 0xf8) == 0xf0)
        {
            rest = 3;
            value = first & 0x07;
            minimum = 0x10000;
        }
        else
        {
            return false;
        }
        if (i + rest > text.size())
        {
            return false;
        }
        while (rest--)
        {
            const auto next = static_cast<unsigned char>(text[i++]);
            if ((next & 0xc0) != 0x80)
            {
                return false;
            }
            value = (value << 6) | (next & 0x3f);
        }
        if (value < minimum || value > 0x10ffff ||
            (value >= 0xd800 && value <= 0xdfff))
        {
            return false;
        }
    }
    return true;
}

std::string acceptKey(std::string_view key)
{
    std::string source(key);
    source += kGuid;
    std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
    unsigned int digest_size = 0;
    if (::EVP_Digest(
            source.data(),
            source.size(),
            digest.data(),
            &digest_size,
            ::EVP_sha1(),
            nullptr
        ) != 1 ||
        digest_size != 20)
    {
        throw std::runtime_error("WebSocket SHA-1 failed");
    }
    std::array<unsigned char, 32> encoded{};
    const auto length = ::EVP_EncodeBlock(
        encoded.data(),
        digest.data(),
        digest_size
    );
    return std::string(
        reinterpret_cast<const char*>(encoded.data()),
        length
    );
}

unsigned char octet(char value)
{
    return static_cast<unsigned char>(value);
}

} // namespace

bool WebSocketSession::validText(std::string_view text)
{
    return validUtf8(text);
}

bool WebSocketSession::validUpgrade(const HttpRequest& request)
{
    const auto upgrade = request.header("upgrade");
    const auto connection = request.header("connection");
    const auto key = request.header("sec-websocket-key");
    const auto version = request.header("sec-websocket-version");
    return request.method == "GET" && request.version == "HTTP/1.1" &&
           request.header("host").has_value() &&
           request.body.empty() && upgrade &&
           hasToken(*upgrade, "websocket") && connection &&
           hasToken(*connection, "upgrade") && version &&
           *version == "13" && key && validKey(*key);
}

WebSocketSession::WebSocketSession(
    Stream& stream,
    const HttpRequest& request,
    std::string buffered,
    std::size_t max_message,
    std::shared_ptr<OutboundBudget> outbound,
    std::chrono::milliseconds write_timeout
)
    : m_stream(stream)
    , m_buffered(std::move(buffered))
    , m_maxMessage(max_message)
    , m_outbound(
          outbound ? std::move(outbound)
                   : OutboundBudget::processDefault()
      )
    , m_writeTimeout(write_timeout)
{
    if (!validUpgrade(request) || max_message == 0 ||
        write_timeout.count() <= 0)
    {
        throw std::invalid_argument(
            "invalid WebSocket upgrade request"
        );
    }
    std::string response = "HTTP/1.1 101 Switching Protocols";
    response += kCrLf;
    response += "Upgrade: websocket";
    response += kCrLf;
    response += "Connection: Upgrade";
    response += kCrLf;
    response += "Sec-WebSocket-Accept: ";
    response += acceptKey(*request.header("sec-websocket-key"));
    response += kCrLf;
    response += kCrLf;
    m_stream.writeAll(std::as_bytes(std::span(response)));
}

std::string WebSocketSession::readBytes(std::size_t count)
{
    std::string result;
    result.reserve(count);
    const auto buffered = std::min(count, m_buffered.size());
    result.append(m_buffered, 0, buffered);
    m_buffered.erase(0, buffered);
    if (result.size() < count)
    {
        const auto old_size = result.size();
        result.resize(count);
        m_stream.readExact(std::as_writable_bytes(
            std::span(result).subspan(old_size)
        ));
    }
    return result;
}

std::optional<WebSocketMessage> WebSocketSession::readMessage()
{
    if (m_closed)
    {
        return std::nullopt;
    }
    for (;;)
    {
        const auto header = readBytes(2);
        const auto first = octet(header[0]);
        const auto second = octet(header[1]);
        const bool final = (first & 0x80) != 0;
        const auto opcode = first & 0x0f;
        if ((first & 0x70) != 0 || (second & 0x80) == 0 ||
            !(opcode == 0 || opcode == 1 || opcode == 2 ||
              opcode == 8 || opcode == 9 || opcode == 10))
        {
            throw std::invalid_argument(
                "invalid WebSocket frame header"
            );
        }
        std::uint64_t length = second & 0x7f;
        if (length == 126)
        {
            const auto extended = readBytes(2);
            length = (octet(extended[0]) << 8) | octet(extended[1]);
            if (length < 126)
            {
                throw std::invalid_argument(
                    "nonminimal WebSocket frame length"
                );
            }
        }
        else if (length == 127)
        {
            const auto extended = readBytes(8);
            length = 0;
            for (const auto byte : extended)
            {
                length = (length << 8) | octet(byte);
            }
            if (length <= 65535 || (length >> 63) != 0)
            {
                throw std::invalid_argument(
                    "invalid WebSocket frame length"
                );
            }
        }
        if (opcode >= 8 && (!final || length > 125))
        {
            throw std::invalid_argument(
                "invalid WebSocket control frame"
            );
        }
        if (opcode < 8 && (length > m_maxMessage ||
                           length > m_maxMessage - m_fragment.size()))
        {
            throw std::invalid_argument("WebSocket message too large"
            );
        }
        const auto mask = readBytes(4);
        auto payload = readBytes(static_cast<std::size_t>(length));
        for (std::size_t i = 0; i < payload.size(); ++i)
        {
            payload[i] ^= mask[i % 4];
        }

        if (opcode == 8)
        {
            if (payload.size() == 1 ||
                (payload.size() > 2 &&
                 !validUtf8(std::string_view(payload).substr(2))))
            {
                throw std::invalid_argument(
                    "invalid WebSocket close frame"
                );
            }
            sendFrame(8, payload);
            m_closed = true;
            return std::nullopt;
        }
        if (opcode == 9)
        {
            sendFrame(10, payload);
            continue;
        }
        if (opcode == 10)
        {
            continue;
        }
        if (opcode == 0)
        {
            if (m_fragmentOpcode == 0)
            {
                throw std::invalid_argument(
                    "unexpected WebSocket continuation"
                );
            }
            m_fragment += payload;
        }
        else
        {
            if (m_fragmentOpcode != 0)
            {
                throw std::invalid_argument(
                    "interleaved WebSocket messages"
                );
            }
            m_fragmentOpcode = opcode;
            m_fragment = std::move(payload);
        }
        if (final)
        {
            auto type = m_fragmentOpcode == 1
                            ? WebSocketMessage::Type::Text
                            : WebSocketMessage::Type::Binary;
            if (type == WebSocketMessage::Type::Text &&
                !validUtf8(m_fragment))
            {
                throw std::invalid_argument("invalid WebSocket UTF-8"
                );
            }
            auto result = WebSocketMessage{
                type,
                std::move(m_fragment)};
            m_fragment.clear();
            m_fragmentOpcode = 0;
            return result;
        }
    }
}

void WebSocketSession::sendFrame(
    std::uint8_t opcode, std::string_view payload
)
{
    if (m_sending.test_and_set(std::memory_order_acquire))
    {
        throw std::system_error(
            ENOBUFS,
            std::generic_category(),
            "concurrent WebSocket send"
        );
    }
    struct SendingGuard
    {
        std::atomic_flag& flag;
        ~SendingGuard()
        {
            flag.clear(std::memory_order_release);
        }
    } sending{m_sending};
    const auto reservation = m_outbound->tryReserve(
        payload.size() + 14
    );
    if (!reservation)
    {
        throw std::system_error(
            ENOBUFS,
            std::generic_category(),
            "WebSocket outbound budget"
        );
    }
    std::string frame;
    frame.push_back(static_cast<char>(0x80 | opcode));
    if (payload.size() <= 125)
    {
        frame.push_back(static_cast<char>(payload.size()));
    }
    else if (payload.size() <= 65535)
    {
        frame.push_back(126);
        frame.push_back(static_cast<char>(payload.size() >> 8));
        frame.push_back(static_cast<char>(payload.size()));
    }
    else
    {
        frame.push_back(127);
        const auto size = static_cast<std::uint64_t>(payload.size());
        for (int shift = 56; shift >= 0; shift -= 8)
        {
            frame.push_back(static_cast<char>(size >> shift));
        }
    }
    frame += payload;
    auto context = std::make_shared<RequestContext>(
        std::chrono::steady_clock::now() + m_writeTimeout
    );
    m_stream.setRequestContext(context);
    try
    {
        m_stream.writeAll(std::as_bytes(std::span(frame)));
    }
    catch (...)
    {
        m_stream.setRequestContext({});
        throw;
    }
    m_stream.setRequestContext({});
}

void WebSocketSession::sendText(std::string_view text)
{
    if (m_closed || text.size() > m_maxMessage || !validUtf8(text))
    {
        throw std::invalid_argument("invalid WebSocket text message");
    }
    sendFrame(1, text);
}

void WebSocketSession::sendBinary(std::string_view data)
{
    if (m_closed || data.size() > m_maxMessage)
    {
        throw std::invalid_argument("invalid WebSocket binary message"
        );
    }
    sendFrame(2, data);
}

void WebSocketSession::close(std::uint16_t code)
{
    if (m_closed)
    {
        return;
    }
    std::array<char, 2> payload{
        static_cast<char>(code >> 8),
        static_cast<char>(code)};
    sendFrame(8, std::string_view(payload.data(), payload.size()));
    m_closed = true;
}

} // namespace sylar