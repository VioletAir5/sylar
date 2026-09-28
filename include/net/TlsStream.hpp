#pragma once

#include <chrono>
#include <cstddef>
#include <memory>
#include <span>
#include <string>

#include <openssl/ssl.h>

#include "net/Socket.hpp"
#include "net/Stream.hpp"

#include "fiber/IOManager.hpp"

namespace sylar
{

class TlsContext
{
public:
    static std::shared_ptr<TlsContext> server(
        const std::string& certificate, const std::string& private_key
    );
    static std::shared_ptr<TlsContext>
    client(const std::string& ca_file = {});

    ~TlsContext();
    TlsContext(const TlsContext&) = delete;
    TlsContext& operator=(const TlsContext&) = delete;

    SSL_CTX* native() const noexcept;
    bool clientMode() const noexcept;

private:
    TlsContext(SSL_CTX* context, bool client) noexcept;

    SSL_CTX* m_context;
    bool m_client;
};

class TlsStream final : public Stream
{
public:
    enum class Role
    {
        Server,
        Client
    };

    TlsStream(
        IOManager& io,
        Socket socket,
        std::shared_ptr<TlsContext> context,
        Role role,
        std::string peer_host = {},
        std::chrono::milliseconds timeout = std::chrono::seconds(30)
    );
    ~TlsStream() override;

    TlsStream(const TlsStream&) = delete;
    TlsStream& operator=(const TlsStream&) = delete;

    void handshake();
    std::size_t readSome(std::span<std::byte> buffer) override;
    std::size_t writeSome(std::span<const std::byte> buffer) override;

    Socket& socket() noexcept;

private:
    int handleRetry(int result);
    void waitFor(IOManager::Event event);

    IOManager& m_io;
    Socket m_socket;
    std::shared_ptr<TlsContext> m_context;
    SSL* m_ssl = nullptr;
    std::chrono::milliseconds m_timeout;
    bool m_handshaken = false;
};

} // namespace sylar
