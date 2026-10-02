#include <poll.h>
#include <unistd.h>

#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string_view>

#include <grpcpp/grpcpp.h>
#include <grpcpp/health_check_service_interface.h>

#include "os/SignalFd.hpp"

#include "rpc/CatalogService.hpp"

namespace
{
unsigned number(std::string_view text, unsigned maximum)
{
    unsigned value = 0;
    const auto [end, error] = std::from_chars(
        text.data(),
        text.data() + text.size(),
        value
    );
    if (error != std::errc{} || end != text.data() + text.size() ||
        value > maximum)
    {
        throw std::invalid_argument("invalid numeric argument");
    }
    return value;
}
} // namespace

int main(int argc, char** argv)
{
    try
    {
        if (argc > 4)
        {
            throw std::invalid_argument(
                "usage: catalog_server [port] [instance-id] "
                "[delay-ms]"
            );
        }
        const unsigned port = argc > 1 ? number(argv[1], 65535)
                                       : 50051;
        const std::string instance = argc > 2 ? argv[2] : "catalog-a";
        const auto delay = std::chrono::milliseconds(
            argc > 3 ? number(argv[3], 5000) : 0
        );
        sylar::SignalFd signals;
        sylar::rpc::CatalogService catalog(instance, delay);
        grpc::EnableDefaultHealthCheckService(true);
        grpc::ServerBuilder builder;
        grpc::ResourceQuota quota;
        quota.SetMaxThreads(64);
        builder.SetResourceQuota(quota);
        int actual_port = 0;
        builder.AddListeningPort(
            std::string(
                std::getenv("SYLAR_LISTEN_ADDRESS")
                    ? std::getenv("SYLAR_LISTEN_ADDRESS")
                    : "127.0.0.1"
            ) + ":" +
                std::to_string(port),
            grpc::InsecureServerCredentials(),
            &actual_port
        );
        builder.SetMaxReceiveMessageSize(64 * 1024);
        builder.SetMaxSendMessageSize(64 * 1024);
        builder.RegisterService(&catalog);
        auto server = builder.BuildAndStart();
        if (!server || actual_port == 0)
        {
            throw std::runtime_error("cannot listen for catalog RPCs"
            );
        }
        std::cout << "catalog_server listening on port "
                  << actual_port << " instance=" << instance
                  << std::endl;
        pollfd descriptor{signals.get(), POLLIN, 0};
        for (;;)
        {
            const int result = ::poll(&descriptor, 1, -1);
            if (result < 0 && errno == EINTR)
            {
                continue;
            }
            if (result <= 0)
            {
                throw std::runtime_error("catalog signal poll failed"
                );
            }
            signalfd_siginfo info{};
            if (::read(signals.get(), &info, sizeof(info)) ==
                sizeof(info))
            {
                break;
            }
        }
        server->Shutdown(
            std::chrono::system_clock::now() + std::chrono::seconds(2)
        );
        server->Wait();
        std::cout << "catalog_server stopped" << std::endl;
    }
    catch (const std::exception& error)
    {
        std::cerr << "catalog_server: " << error.what() << std::endl;
        return 1;
    }
}
