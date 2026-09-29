#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "net/TlsStream.hpp"

#include "fiber/IOManager.hpp"
#include "http/Uri.hpp"
#include "http/WebSocket.hpp"

namespace sylar
{

class WebSocketClient
{
public:
    static std::unique_ptr<WebSocketClient> connect(
        IOManager& io,
        const Uri& uri,
        std::chrono::milliseconds timeout = std::chrono::seconds(30),
        std::shared_ptr<TlsContext> tls = {},
        std::size_t max_message = 1024 * 1024
    );

    WebSocketClient(const WebSocketClient&) = delete;
    WebSocketClient& operator=(const WebSocketClient&) = delete;

    std::optional<WebSocketMessage> readMessage();
    void sendText(std::string_view text);
    void sendBinary(std::string_view data);
    void close(std::uint16_t code = 1000);

private:
    WebSocketClient(
        std::unique_ptr<Stream> stream,
        std::string buffered,
        std::size_t max_message
    );

    std::string readBytes(std::size_t count);
    void sendFrame(std::uint8_t opcode, std::string_view payload);

    std::unique_ptr<Stream> m_stream;
    std::string m_buffered;
    std::size_t m_maxMessage;
    std::string m_fragment;
    std::uint8_t m_fragmentOpcode = 0;
    bool m_closeSent = false;
    bool m_closed = false;
};

} // namespace syla
