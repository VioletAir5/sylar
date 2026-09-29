#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

#include "net/TlsStream.hpp"

#include "fiber/IOManager.hpp"
#include "http/Uri.hpp"
#include "http/WebSocketClient.hpp"

int main(int argc, char** argv)
{
    try
    {
        const bool ca_mode = argc == 5 &&
                             std::string_view(argv[3]) == "--ca";
        if (argc != 3 && !ca_mode)
        {
            throw std::invalid_argument(
                "usage: ws_client ws://host:port/path text "
                "[--ca ca.pem]"
            );
        }
        const auto uri = sylar::Uri::parse(argv[1]);
        sylar::IOManager io;
        std::string reply;
        io.schedule(
            [&]
            {
                auto tls = ca_mode
                               ? sylar::TlsContext::client(argv[4])
                               : std::shared_ptr<sylar::TlsContext>{};
                auto client = sylar::WebSocketClient::connect(
                    io,
                    uri,
                    std::chrono::seconds(5),
                    std::move(tls)
                );
                client->sendText(argv[2]);
                auto message = client->readMessage();
                if (!message ||
                    message->type !=
                        sylar::WebSocketMessage::Type::Text)
                {
                    throw std::runtime_error("missing text reply");
                }
                reply = std::move(message->payload);
                client->close();
                client->readMessage();
            }
        );
        io.run();
        std::cout << reply << '\n';
    }
    catch (const std::exception& error)
    {
        std::cerr << "ws_client: " << error.what() << '\n';
        return 1;
    }
}
