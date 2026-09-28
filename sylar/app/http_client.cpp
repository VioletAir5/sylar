#include <chrono>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string_view>

#include "net/TlsStream.hpp"

#include "fiber/IOManager.hpp"
#include "http/HttpClient.hpp"
#include "http/Uri.hpp"

int main(int argc, char** argv)
{
    try
    {
        if (argc != 2 &&
            !(argc == 4 && std::string_view(argv[2]) == "--ca"))
        {
            throw std::invalid_argument(
                "usage: http_client URL [--ca ca.pem]"
            );
        }
        const auto uri = sylar::Uri::parse(argv[1]);
        std::shared_ptr<sylar::TlsContext> tls;
        if (argc == 4)
        {
            if (uri.scheme != "https")
            {
                throw std::invalid_argument("--ca requires HTTPS");
            }
            tls = sylar::TlsContext::client(argv[3]);
        }
        sylar::IOManager io;
        sylar::HttpResponse response;
        io.schedule(
            [&]
            {
                response = sylar::HttpClient::request(
                    io,
                    uri,
                    "GET",
                    {},
                    {},
                    std::chrono::seconds(5),
                    tls
                );
            }
        );
        io.run();
        std::cout << response.status << " " << response.reason << "\n"
                  << response.body;
    }
    catch (const std::exception& error)
    {
        std::cerr << "http_client: " << error.what() << std::endl;
        return 1;
    }
}