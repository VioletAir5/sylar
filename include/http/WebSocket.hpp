#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "net/Stream.hpp"

#include "http/Http.hpp"

namespace sylar
{

struct WebSocketMessage
{
    enum class Type
    {
        Text,
        Binary
    };

    Type type;
    std::string payload;
};

class WebSocketSession
{
public:
    static bool validUpgrade(const HttpRequest& request);
    static bool validText(std::string_view text);

    WebSocketSession(
        Stream& stream,
        const HttpRequest& request,
        std::string buffered = {},
        std::size_t max_message = 1024 * 1024
    );

    // A null value means the peer completed the close handshake.
    std::optional<WebSocketMessage> readMessage();
    void sendText(std::string_view text);
    void sendBinary(std::string_view data);
    void close(std::uint16_t code = 1000);

private:
    std::string readBytes(std::size_t count);
    void sendFrame(std::uint8_t opcode, std::string_view payload);

    Stream& m_stream;
    std::string m_buffered;
    std::size_t m_maxMessage;
    std::string m_fragment;
    std::uint8_t m_fragmentOpcode = 0;
    bool m_closed = false;
};

} // namespace sylar