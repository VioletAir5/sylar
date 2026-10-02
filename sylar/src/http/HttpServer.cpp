#include "http/HttpServer.hpp"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <utility>

#include "net/Socket.hpp"
#include "net/SocketStream.hpp"
#include "net/TlsStream.hpp"

#include "fiber/RequestContext.hpp"
#include "http/Http.hpp"
#include "http/WebSocket.hpp"

namespace sylar
{

class WebSocketRouteRegistry
{
public:
    void add(std::string path, HttpServer::WebSocketHandler handler)
    {
        if (path.empty() || path.front() != '/' || !handler)
        {
            throw std::invalid_argument("invalid WebSocket route");
        }
        std::lock_guard lock(m_mutex);
        m_handlers.insert_or_assign(
            std::move(path),
            std::move(handler)
        );
        m_hasRoutes.store(true, std::memory_order_release);
    }

    std::optional<HttpServer::WebSocketHandler>
    find(std::string_view target) const
    {
        if (!m_hasRoutes.load(std::memory_order_acquire))
        {
            return std::nullopt;
        }
        const auto query = target.find('?');
        std::lock_guard lock(m_mutex);
        const auto it = m_handlers.find(
            std::string(target.substr(0, query))
        );
        if (it == m_handlers.end())
        {
            return std::nullopt;
        }
        return it->second;
    }

private:
    mutable std::mutex m_mutex;
    std::atomic_bool m_hasRoutes{false};
    std::unordered_map<std::string, HttpServer::WebSocketHandler>
        m_handlers;
};

namespace
{

TcpServer::Handler makeHandler(
    std::shared_ptr<ServletDispatch> dispatch,
    std::shared_ptr<WebSocketRouteRegistry> ws_routes,
    std::shared_ptr<std::atomic_bool> draining,
    std::shared_ptr<TlsContext> tls,
    std::shared_ptr<RequestLimiter> limiter
)
{
    if (!dispatch)
    {
        throw std::invalid_argument("null servlet dispatch");
    }
    return [dispatch = std::move(dispatch),
            ws_routes = std::move(ws_routes),
            draining = std::move(draining),
            tls = std::move(tls),
            limiter = std::move(limiter)](IOManager& io, int fd)
    {
        try
        {
            std::unique_ptr<Stream> stream;
            if (tls)
            {
                stream = std::make_unique<TlsStream>(
                    io,
                    Socket::borrow(fd),
                    tls,
                    TlsStream::Role::Server
                );
            }
            else
            {
                stream = std::make_unique<SocketStream>(
                    io,
                    Socket::borrow(fd),
                    std::chrono::seconds(30)
                );
            }
            HttpSession session(*stream);
            HttpRequest request;
            while (true)
            {
                auto request_context = std::make_shared<
                    RequestContext>(
                    std::chrono::steady_clock::now() +
                    std::chrono::seconds(30)
                );
                stream->setRequestContext(request_context);
                try
                {
                    if (!session.readRequest(request))
                    {
                        return;
                    }
                }
                catch (const std::invalid_argument&)
                {
                    HttpResponse bad;
                    bad.status = 400;
                    bad.reason = "Bad Request";
                    bad.body = "400 Bad Request\n";
                    session.sendResponse(bad, false);
                    return;
                }

                request.context = request_context;
                if (const auto handler = ws_routes->find(
                        request.target
                    ))
                {
                    if (!WebSocketSession::validUpgrade(request))
                    {
                        HttpResponse bad;
                        bad.status = 400;
                        bad.reason = "Bad Request";
                        bad.body = "400 Bad Request\n";
                        session.sendResponse(bad, false);
                        return;
                    }
                    stream->setRequestContext({});
                    WebSocketSession websocket(
                        *stream,
                        request,
                        session.takeBuffered()
                    );
                    for (;;)
                    {
                        std::optional<WebSocketMessage> message;
                        try
                        {
                            message = websocket.readMessage();
                        }
                        catch (const std::invalid_argument&)
                        {
                            websocket.close(1002);
                            return;
                        }
                        if (!message)
                        {
                            return;
                        }
                        (*handler)(websocket, *message);
                    }
                }

                std::optional<RequestLimiter::Permit> permit;
                if (limiter)
                {
                    permit = limiter->tryAcquire();
                    if (!permit)
                    {
                        HttpResponse overloaded;
                        overloaded.status = 503;
                        overloaded.reason = "Service Unavailable";
                        overloaded.headers["Retry-After"] = "1";
                        overloaded.body = "503 overloaded\n";
                        session.sendResponse(
                            overloaded,
                            false,
                            request.method == "HEAD"
                        );
                        return;
                    }
                }
                HttpResponse response;
                try
                {
                    response = dispatch->handle(request);
                }
                catch (const std::exception&)
                {
                    response.status = 500;
                    response.reason = "Internal Server Error";
                    response.body = "500 Internal Server Error\n";
                }
                const bool keep_alive = request.keepAlive() &&
                                        !draining->load(
                                            std::memory_order_acquire
                                        );
                session.sendResponse(
                    response,
                    keep_alive,
                    request.method == "HEAD"
                );
                if (!keep_alive)
                {
                    return;
                }
            }
        }
        catch (const std::exception&)
        {
            // Peer closure, timeout or I/O cancellation ends a
            // session.
        }
    };
}

} // namespace

HttpServer::HttpServer(
    IOManager& io,
    std::shared_ptr<ServletDispatch> dispatch,
    std::shared_ptr<TlsContext> tls,
    std::shared_ptr<RequestLimiter> limiter
)
    : m_wsRoutes(std::make_shared<WebSocketRouteRegistry>())
    , m_draining(std::make_shared<std::atomic_bool>(false))
    , m_limiter(std::move(limiter))
    , m_tcp(
          io,
          makeHandler(
              std::move(dispatch),
              m_wsRoutes,
              m_draining,
              std::move(tls),
              m_limiter
          )
      )
{
}

HttpServer::HttpServer(
    IOManager& acceptor,
    IOManagerPool& workers,
    std::shared_ptr<ServletDispatch> dispatch,
    std::shared_ptr<TlsContext> tls,
    std::shared_ptr<RequestLimiter> limiter
)
    : m_wsRoutes(std::make_shared<WebSocketRouteRegistry>())
    , m_draining(std::make_shared<std::atomic_bool>(false))
    , m_limiter(std::move(limiter))
    , m_tcp(
          acceptor,
          workers,
          makeHandler(
              std::move(dispatch),
              m_wsRoutes,
              m_draining,
              std::move(tls),
              m_limiter
          )
      )
{
}

void HttpServer::addWebSocket(
    std::string path, WebSocketHandler handler
)
{
    m_wsRoutes->add(std::move(path), std::move(handler));
}

void HttpServer::start(const std::string& ipv4, std::uint16_t port)
{
    m_tcp.start(ipv4, port);
}

void HttpServer::start(const Address& address)
{
    m_tcp.start(address);
}

void HttpServer::start(const std::vector<Address>& addresses)
{
    m_tcp.start(addresses);
}

void HttpServer::stop()
{
    beginDrain();
    m_tcp.stop();
}

void HttpServer::beginDrain() noexcept
{
    m_draining->store(true, std::memory_order_release);
}

bool HttpServer::draining() const noexcept
{
    return m_draining->load(std::memory_order_acquire);
}

void HttpServer::setMaxConnections(std::size_t limit)
{
    m_tcp.setMaxConnections(limit);
}

std::size_t HttpServer::maxConnections() const noexcept
{
    return m_tcp.maxConnections();
}

std::size_t HttpServer::activeConnections() const noexcept
{
    return m_tcp.activeConnections();
}

std::size_t HttpServer::rejectedConnections() const noexcept
{
    return m_tcp.rejectedConnections();
}

RequestLimiter::Snapshot HttpServer::requestStats() const noexcept
{
    return m_limiter ? m_limiter->snapshot()
                     : RequestLimiter::Snapshot{};
}

std::uint16_t HttpServer::localPort() const
{
    return m_tcp.localPort();
}

std::vector<Address> HttpServer::localAddresses() const
{
    return m_tcp.localAddresses();
}

} // namespace sylar
