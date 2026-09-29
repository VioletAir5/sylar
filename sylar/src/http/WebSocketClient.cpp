#include "http/WebSocketClient.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

#include <openssl/evp.h>
#include <openssl/rand.h>

#include "net/Address.hpp"
#include "net/Socket.hpp"
#include "net/SocketStream.hpp"

namespace sylar
{
namespace
{

constexpr std::string_view
    kGuid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

std::string lower(std::string_view text)
{
    std::string result(text);
    std::transform(
        result.begin(),
        result.end(),
        result.begin(),
        [](unsigned char ch)
        { return static_cast<char>(std::tolower(ch)); }
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
    return text.substr(
        first,
        text.find_last_not_of(" \t") - first + 1
    );
}

bool hasToken(std::string_view value, std::string_view expected)
{
    while (!value.empty())
    {
        const auto comma = value.find(',');
        if (lower(trim(value.substr(0, comma))) == expected)
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

std::string randomKey()
{
    std::array<unsigned char, 16> bytes{};
    if (::RAND_bytes(bytes.data(), bytes.size()) != 1)
    {
        throw std::runtime_error("WebSocket nonce generation failed");
    }
    std::array<unsigned char, 25> encoded{};
    const auto count = ::EVP_EncodeBlock(
        encoded.data(),
        bytes.data(),
        bytes.size()
    );
    return std::string(
        reinterpret_cast<const char*>(encoded.data()),
        count
    );
}

std::string expectedAccept(std::string_view key)
{
    std::string source(key);
    source += kGuid;
    std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
    unsigned int length = 0;
    if (::EVP_Digest(
            source.data(),
            source.size(),
            digest.data(),
            &length,
            ::EVP_sha1(),
            nullptr
        ) != 1 ||
        length != 20)
    {
        throw std::runtime_error("WebSocket SHA-1 failed");
    }
    std::array<unsigned char, 32> encoded{};
    const auto count = ::EVP_EncodeBlock(
        encoded.data(),
        digest.data(),
        length
    );
    return std::string(
        reinterpret_cast<const char*>(encoded.data()),
        count
    );
}

std::string readHandshake(Stream& stream, std::string_view key)
{
    std::string buffer;
    std::array<std::byte, 4096> chunk{};
    auto boundary = buffer.find("\r\n\r\n");
    while (boundary == std::string::npos)
    {
        if (buffer.size() > 16 * 1024)
        {
            throw std::invalid_argument(
                "WebSocket response header too large"
            );
        }
        const auto count = stream.readSome(chunk);
        if (count == 0)
        {
            throw std::invalid_argument("WebSocket handshake EOF");
        }
        buffer.append(
            reinterpret_cast<const char*>(chunk.data()),
            count
        );
        boundary = buffer.find("\r\n\r\n");
    }
    if (boundary + 4 > 16 * 1024)
    {
        throw std::invalid_argument(
            "WebSocket response header too large"
        );
    }
    const std::string_view head(buffer.data(), boundary);
    const auto first_end = head.find("\r\n");
    const auto status = head.substr(0, first_end);
    if (first_end == std::string_view::npos || status.size() < 12 ||
        status.substr(0, 12) != "HTTP/1.1 101" ||
        (status.size() > 12 && status[12] != ' '))
    {
        throw std::invalid_argument(
            "WebSocket upgrade status is not 101"
        );
    }
    std::unordered_map<std::string, std::string> headers;
    std::size_t position = first_end + 2;
    while (position < head.size())
    {
        const auto end = head.find("\r\n", position);
        const auto line = head.substr(
            position,
            end == std::string_view::npos ? head.size() - position
                                          : end - position
        );
        const auto colon = line.find(':');
        if (colon == std::string_view::npos || colon == 0)
        {
            throw std::invalid_argument(
                "invalid WebSocket upgrade header"
            );
        }
        if (!headers
                 .emplace(
                     lower(line.substr(0, colon)),
                     trim(line.substr(colon + 1))
                 )
                 .second)
        {
            throw std::invalid_argument(
                "duplicate WebSocket upgrade header"
            );
        }
        if (end == std::string_view::npos)
        {
            break;
        }
        position = end + 2;
    }
    const auto upgrade = headers.find("upgrade");
    const auto connection = headers.find("connection");
    const auto accept = headers.find("sec-websocket-accept");
    if (upgrade == headers.end() ||
        !hasToken(upgrade->second, "websocket") ||
        connection == headers.end() ||
        !hasToken(connection->second, "upgrade") ||
        accept == headers.end() ||
        accept->second != expectedAccept(key) ||
        headers.contains("sec-websocket-extensions") ||
        headers.contains("sec-websocket-protocol"))
    {
        throw std::invalid_argument(
            "invalid WebSocket upgrade response"
        );
    }
    buffer.erase(0, boundary + 4);
    return buffer;
}

unsigned char octet(char value)
{
    return static_cast<unsigned char>(value);
}

} // namespace

WebSocketClient::WebSocketClient(
    std::unique_ptr<Stream> stream,
    std::string buffered,
    std::size_t max_message
)
    : m_stream(std::move(stream))
    , m_buffered(std::move(buffered))
    , m_maxMessage(max_message)
{
}

std::unique_ptr<WebSocketClient> WebSocketClient::connect(
    IOManager& io,
    const Uri& uri,
    std::chrono::milliseconds timeout,
    std::shared_ptr<TlsContext> tls,
    std::size_t max_message
)
{
    if ((uri.scheme != "ws" && uri.scheme != "wss") ||
        uri.host.empty() || uri.port == 0 || max_message == 0 ||
        uri.host.find_first_of(" \t\r\n") != std::string::npos ||
        uri.target.empty() || uri.target.front() != '/' ||
        uri.target.find_first_of(" \t\r\n") != std::string::npos ||
        (uri.scheme == "ws" && tls))
    {
        throw std::invalid_argument(
            "invalid WebSocket URI or message limit"
        );
    }
    const auto addresses = Address::resolve(
        uri.host,
        std::to_string(uri.port)
    );
    if (addresses.empty())
    {
        throw std::runtime_error("WebSocket host has no addresses");
    }
    auto socket = Socket::tcp(addresses.front().family());
    socket.connect(io, addresses.front(), timeout);
    std::unique_ptr<Stream> stream;
    if (uri.scheme == "wss")
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
    const auto key = randomKey();
    const std::string
        request = "GET " + uri.target +
                  " HTTP/1.1\r\nHost: " + uri.authority() +
                  "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                  "Sec-WebSocket-Version: 13\r\nSec-WebSocket-Key: " +
                  key + "\r\n\r\n";
    stream->writeAll(std::as_bytes(std::span(request)));
    auto buffered = readHandshake(*stream, key);
    return std::unique_ptr<WebSocketClient>(new WebSocketClient(
        std::move(stream),
        std::move(buffered),
        max_message
    ));
}

std::string WebSocketClient::readBytes(std::size_t count)
{
    std::string result;
    result.reserve(count);
    const auto buffered = std::min(count, m_buffered.size());
    result.append(m_buffered, 0, buffered);
    m_buffered.erase(0, buffered);
    if (result.size() < count)
    {
        const auto old = result.size();
        result.resize(count);
        m_stream->readExact(
            std::as_writable_bytes(std::span(result).subspan(old))
        );
    }
    return result;
}

std::optional<WebSocketMessage> WebSocketClient::readMessage()
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
        if ((first & 0x70) != 0 || (second & 0x80) != 0 ||
            !(opcode == 0 || opcode == 1 || opcode == 2 ||
              opcode == 8 || opcode == 9 || opcode == 10))
        {
            throw std::invalid_argument(
                "invalid WebSocket server frame"
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
            for (char byte : extended)
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
        auto payload = readBytes(static_cast<std::size_t>(length));
        if (opcode == 8)
        {
            if (payload.size() == 1 ||
                (payload.size() > 2 &&
                 !WebSocketSession::validText(
                     std::string_view(payload).substr(2)
                 )))
            {
                throw std::invalid_argument(
                    "invalid WebSocket close frame"
                );
            }
            if (!m_closeSent)
            {
                sendFrame(8, payload);
                m_closeSent = true;
            }
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
            const auto type = m_fragmentOpcode == 1
                                  ? WebSocketMessage::Type::Text
                                  : WebSocketMessage::Type::Binary;
            if (type == WebSocketMessage::Type::Text &&
                !WebSocketSession::validText(m_fragment))
            {
                throw std::invalid_argument("invalid WebSocket UTF-8"
                );
            }
            auto message = WebSocketMessage{
                type,
                std::move(m_fragment)};
            m_fragment.clear();
            m_fragmentOpcode = 0;
            return message;
        }
    }
}

void WebSocketClient::sendFrame(
    std::uint8_t opcode, std::string_view payload
)
{
    std::array<unsigned char, 4> mask{};
    if (::RAND_bytes(mask.data(), mask.size()) != 1)
    {
        throw std::runtime_error("WebSocket mask generation failed");
    }
    std::string frame;
    frame.push_back(static_cast<char>(0x80 | opcode));
    if (payload.size() <= 125)
    {
        frame.push_back(static_cast<char>(0x80 | payload.size()));
    }
    else if (payload.size() <= 65535)
    {
        frame.push_back(static_cast<char>(0x80 | 126));
        frame.push_back(static_cast<char>(payload.size() >> 8));
        frame.push_back(static_cast<char>(payload.size()));
    }
    else
    {
        frame.push_back(static_cast<char>(0x80 | 127));
        const auto length = static_cast<std::uint64_t>(payload.size()
        );
        for (int shift = 56; shift >= 0; shift -= 8)
        {
            frame.push_back(static_cast<char>(length >> shift));
        }
    }
    frame.append(
        reinterpret_cast<const char*>(mask.data()),
        mask.size()
    );
    for (std::size_t i = 0; i < payload.size(); ++i)
    {
        frame.push_back(
            static_cast<char>(octet(payload[i]) ^ mask[i % 4])
        );
    }
    m_stream->writeAll(std::as_bytes(std::span(frame)));
}

void WebSocketClient::sendText(std::string_view text)
{
    if (m_closeSent || text.size() > m_maxMessage ||
        !WebSocketSession::validText(text))
    {
        throw std::invalid_argument("invalid WebSocket text message");
    }
    sendFrame(1, text);
}

void WebSocketClient::sendBinary(std::string_view data)
{
    if (m_closeSent || data.size() > m_maxMessage)
    {
        throw std::invalid_argument("invalid WebSocket binary message"
        );
    }
    sendFrame(2, data);
}

void WebSocketClient::close(std::uint16_t code)
{
    if (m_closeSent)
    {
        return;
    }
    const std::array<char, 2> payload{
        static_cast<char>(code >> 8),
        static_cast<char>(code)};
    sendFrame(8, std::string_view(payload.data(), payload.size()));
    m_closeSent = true;
}

} // namespace syla
