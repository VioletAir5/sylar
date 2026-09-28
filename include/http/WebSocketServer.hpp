#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "net/TcpServer.hpp"
#include "net/TlsStream.hpp"

#include "fiber/IOManager.hpp"
#include "fiber/IOManagerPool.hpp"
#include "http/WebSocket.hpp"

namespace sylar
{

class WebSocketServer
{
public:
    using Handler = std::function<
        void(WebSocketSession&, const WebSocketMessage&)>;

    WebSocketServer(
        IOManager& io,
        Handler handler,
        std::string path = "/ws",
        std::shared_ptr<TlsContext> tls = {}
    );
    WebSocketServer(
        IOManager& acceptor,
        IOManagerPool& workers,
        Handler handler,
        std::string path = "/ws",
        std::shared_ptr<TlsContext> tls = {}
    );

    void start(const Address& address);
    void start(const std::vector<Address>& addresses);
    void stop();
    std::uint16_t localPort() const;

private:
    TcpServer m_tcp;
};

} // namespace sylar