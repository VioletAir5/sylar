#include <sys/signalfd.h>
#include <unistd.h>

#include <charconv>
#include <cstdint>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string_view>

#include "os/SignalFd.hpp"

#include "fiber/IOManager.hpp"
#include "fiber/IOManagerPool.hpp"
#include "http/WebSocketServer.hpp"

namespace
{

unsigned positive(std::string_view text)
{
    unsigned value = 0;
    const auto [end, error] = std::from_chars(
        text.data(),
        text.data() + text.size(),
        value
    );
    if (error != std::errc{} || end != text.data() + text.size() ||
        value == 0)
    {
        throw std::invalid_argument("expected positive number");
    }
    return value;
}

} // namespace

int main(int argc, char** argv)
{
    try
    {
        const bool tls_mode = argc == 6 &&
                              std::string_view(argv[3]) == "--tls";
        if (argc > 3 && !tls_mode)
        {
            throw std::invalid_argument(
                "usage: ws_echo_server [port] [workers] "
                "[--tls cert.pem key.pem]"
            );
        }
        const auto port = argc > 1 ? positive(argv[1]) : 8081;
        const auto workers = argc > 2 ? positive(argv[2]) : 4;
        if (port > 65535 || workers > 128)
        {
            throw std::invalid_argument("invalid port or worker count"
            );
        }

        sylar::SignalFd signals;
        sylar::IOManager io;
        sylar::IOManagerPool pool(workers);
        auto tls = tls_mode
                       ? sylar::TlsContext::server(argv[4], argv[5])
                       : std::shared_ptr<sylar::TlsContext>{};
        sylar::WebSocketServer server(
            io,
            pool,
            [](sylar::WebSocketSession& session,
               const sylar::WebSocketMessage& message)
            {
                if (message.type ==
                    sylar::WebSocketMessage::Type::Text)
                {
                    session.sendText(message.payload);
                }
                else
                {
                    session.sendBinary(message.payload);
                }
            },
            "/ws",
            std::move(tls)
        );
        server.start(sylar::Address::ipv4(
            "0.0.0.0",
            static_cast<std::uint16_t>(port)
        ));
        std::cout << (tls_mode ? "wss" : "ws")
                  << "_echo_server listening on port "
                  << server.localPort() << " workers=" << workers
                  << std::endl;
        io.schedule(
            [&]
            {
                while (io.waitReadable(signals.get()) ==
                       sylar::IOManager::WaitResult::Ready)
                {
                    signalfd_siginfo info{};
                    if (::read(signals.get(), &info, sizeof(info)) ==
                        sizeof(info))
                    {
                        server.stop();
                        io.requestStop();
                        return;
                    }
                }
            }
        );
        io.runForever();
        server.stop();
        pool.stop();
    }
    catch (const std::exception& error)
    {
        std::cerr << "ws_echo_server: " << error.what() << std::endl;
        return 1;
    }
}