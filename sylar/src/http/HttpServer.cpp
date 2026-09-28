#include "http/HttpServer.hpp"

#include <cerrno>
#include <chrono>
#include <memory>
#include <stdexcept>
#include <system_error>
#include <utility>

#include "net/Socket.hpp"
#include "net/SocketStream.hpp"
#include "net/TlsStream.hpp"

#include "http/Http.hpp"

namespace sylar
{
namespace
{

TcpServer::Handler makeHandler(
    std::shared_ptr<ServletDispatch> dispatch,
    std::shared_ptr<TlsContext> tls
)
{
    if (!dispatch)
    {
        throw std::invalid_argument("null servlet dispatch");
    }
    return [dispatch = std::move(dispatch),
            tls = std::move(tls)](IOManager& io, int fd)
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

                const bool keep_alive = request.keepAlive();
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
                session.sendResponse(response, keep_alive);
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
    std::shared_ptr<TlsContext> tls
)
    : m_tcp(io, makeHandler(std::move(dispatch), std::move(tls)))
{
}

HttpServer::HttpServer(
    IOManager& acceptor,
    IOManagerPool& workers,
    std::shared_ptr<ServletDispatch> dispatch,
    std::shared_ptr<TlsContext> tls
)
    : m_tcp(
          acceptor,
          workers,
          makeHandler(std::move(dispatch), std::move(tls))
      )
{
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
    m_tcp.stop();
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
