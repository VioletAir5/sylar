#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "net/Address.hpp"

#include "fiber/IOManager.hpp"
#include "fiber/IOManagerPool.hpp"

namespace sylar
{

// Nonblocking TCP or Unix listener. Handler borrows the accepted
// fd for the duration of its Fiber; TcpServer closes it on return.
class TcpServer
{
public:
    using Handler = std::function<void(IOManager&, int)>;

    TcpServer(IOManager& io, Handler handler);
    TcpServer(
        IOManager& acceptor, IOManagerPool& workers, Handler handler
    );
    ~TcpServer();

    TcpServer(const TcpServer&) = delete;
    TcpServer& operator=(const TcpServer&) = delete;
    TcpServer(TcpServer&&) = delete;
    TcpServer& operator=(TcpServer&&) = delete;

    // Port zero asks the kernel to choose an available port.
    // A stopped instance cannot be restarted.
    void start(const std::string& ipv4, std::uint16_t port);
    void start(const Address& address);
    void stop();
    std::uint16_t localPort() const;
    bool running() const;

private:
    struct State;
    static void acceptLoop(const std::shared_ptr<State>& state);

    std::shared_ptr<State> m_state;
};

} // namespace sylar
