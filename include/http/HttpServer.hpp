#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "net/TcpServer.hpp"

#include "fiber/IOManager.hpp"
#include "fiber/IOManagerPool.hpp"
#include "http/Servlet.hpp"

namespace sylar
{

class HttpServer
{
public:
    HttpServer(
        IOManager& io, std::shared_ptr<ServletDispatch> dispatch
    );
    HttpServer(
        IOManager& acceptor,
        IOManagerPool& workers,
        std::shared_ptr<ServletDispatch> dispatch
    );

    void start(const std::string& ipv4, std::uint16_t port);
    void start(const Address& address);
    void stop();
    std::uint16_t localPort() const;

private:
    TcpServer m_tcp;
};

} // namespace sylar
