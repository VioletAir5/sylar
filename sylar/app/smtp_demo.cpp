#include <charconv>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>

#include "net/TlsStream.hpp"

#include "email/SmtpClient.hpp"
#include "fiber/IOManager.hpp"

int main(int argc, char** argv)
{
    try
    {
        const bool tls_mode = argc == 8 &&
                              std::string_view(argv[6]) == "--tls";
        const bool starttls_mode = argc == 8 &&
                                   std::string_view(argv[6]) ==
                                       "--starttls";
        if (argc != 6 && !tls_mode && !starttls_mode)
        {
            throw std::invalid_argument(
                "usage: smtp_demo HOST PORT FROM TO "
                "message.txt [--tls ca.pem | --starttls ca.pem]"
            );
        }
        unsigned port = 0;
        const std::string_view port_text(argv[2]);
        const auto [end, error] = std::from_chars(
            port_text.data(),
            port_text.data() + port_text.size(),
            port
        );
        if (error != std::errc{} ||
            end != port_text.data() + port_text.size() || port == 0 ||
            port > 65535)
        {
            throw std::invalid_argument("invalid SMTP port");
        }
        std::ifstream file(argv[5], std::ios::binary);
        if (!file)
        {
            throw std::runtime_error("cannot open message file");
        }
        sylar::SmtpMessage message{
            argv[3],
            {argv[4]},
            std::string(
                std::istreambuf_iterator<char>(file),
                std::istreambuf_iterator<char>()
            )};
        std::shared_ptr<sylar::TlsContext> tls;
        if (argc == 8)
        {
            tls = sylar::TlsContext::client(argv[7]);
        }
        sylar::IOManager io;
        io.schedule(
            [&]
            {
                sylar::SmtpClient::send(
                    io,
                    argv[1],
                    static_cast<std::uint16_t>(port),
                    message,
                    "localhost",
                    std::chrono::seconds(5),
                    tls,
                    starttls_mode
                );
            }
        );
        io.run();
        std::cout << "message accepted" << std::endl;
    }
    catch (const std::exception& error)
    {
        std::cerr << "smtp_demo: " << error.what() << std::endl;
        return 1;
    }
}