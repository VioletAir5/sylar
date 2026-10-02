#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "net/TcpServer.hpp"
#include "net/TlsStream.hpp"

#include "fiber/IOManager.hpp"
#include "fiber/IOManagerPool.hpp"
#include "http/Servlet.hpp"
#include "http/WebSocket.hpp"

namespace sylar
{

class WebSocketRouteRegistry;

class HttpServer
{
public:
    using WebSocketHandler = std::function<
        void(WebSocketSession&, const WebSocketMessage&)>;

    HttpServer(
        IOManager& io,
        std::shared_ptr<ServletDispatch> dispatch,
        std::shared_ptr<TlsContext> tls = {}
    );
    HttpServer(
        IOManager& acceptor,
        IOManagerPool& workers,
        std::shared_ptr<ServletDispatch> dispatch,
        std::shared_ptr<TlsContext> tls = {}
    );

    void addWebSocket(std::string path, WebSocketHandler handler);
    void start(const std::string& ipv4, std::uint16_t port);
    void start(const Address& address);
    void start(const std::vector<Address>& addresses);
    void stop();
    // During drain, completed HTTP responses close their connections.
    void beginDrain() noexcept;
    bool draining() const noexcept;
    void setMaxConnections(std::size_t limit);
    std::size_t maxConnections() const noexcept;
    std::size_t activeConnections() const noexcept;
    std::size_t rejectedConnections() const noexcept;
    std::uint16_t localPort() const;
    std::vector<Address> localAddresses() const;

private:
    std::shared_ptr<WebSocketRouteRegistry> m_wsRoutes;
    std::shared_ptr<std::atomic_bool> m_draining;
    TcpServer m_tcp;
};

} // namespace sylar
