#include "http/WebSocketServer.hpp"

#include <chrono>
#include <memory>
#include <stdexcept>
#include <utility>

#include "net/Socket.hpp"
#include "net/SocketStream.hpp"

#include "http/Http.hpp"

namespace sylar
{
namespace
{

TcpServer::Handler makeHandler(
    WebSocketServer::Handler handler,
    std::string path,
    std::shared_ptr<TlsContext> tls
)
{
    if (!handler || path.empty() || path.front() != '/')
    {
        throw std::invalid_argument(
            "invalid WebSocket server handler or path"
        );
    }
    return [handler = std::move(handler),
            path = std::move(path),
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
            HttpSession http(*stream);
            HttpRequest request;
            if (!http.readRequest(request))
            {
                return;
            }
            if (request.target != path ||
                !WebSocketSession::validUpgrade(request))
            {
                HttpResponse response;
                response.status = 400;
                response.reason = "Bad Request";
                response.body = "400 Bad Request";
                http.sendResponse(response, false);
                return;
            }
            WebSocketSession session(
                *stream,
                request,
                http.takeBuffered()
            );
            for (;;)
            {
                std::optional<WebSocketMessage> message;
                try
                {
                    message = session.readMessage();
                }
                catch (const std::invalid_argument&)
                {
                    session.close(1002);
                    return;
                }
                if (!message)
                {
                    return;
                }
                handler(session, *message);
            }
        }
        catch (const std::exception&)
        {
            // A malformed or closed peer ends this connection.
        }
    };
}

} // namespace

WebSocketServer::WebSocketServer(
    IOManager& io,
    Handler handler,
    std::string path,
    std::shared_ptr<TlsContext> tls
)
    : m_tcp(
          io,
          makeHandler(
              std::move(handler), std::move(path), std::move(tls)
          )
      )
{
}

WebSocketServer::WebSocketServer(
    IOManager& acceptor,
    IOManagerPool& workers,
    Handler handler,
    std::string path,
    std::shared_ptr<TlsContext> tls
)
    : m_tcp(
          acceptor,
          workers,
          makeHandler(
              std::move(handler), std::move(path), std::move(tls)
          )
      )
{
}

void WebSocketServer::start(const Address& address)
{
    m_tcp.start(address);
}

void WebSocketServer::start(const std::vector<Address>& addresses)
{
    m_tcp.start(addresses);
}

void WebSocketServer::stop()
{
    m_tcp.stop();
}

std::uint16_t WebSocketServer::localPort() const
{
    return m_tcp.localPort();
}

} // namespace sylar