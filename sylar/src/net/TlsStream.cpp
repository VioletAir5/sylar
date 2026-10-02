#include "net/TlsStream.hpp"

#include <arpa/inet.h>

#include <cerrno>
#include <stdexcept>
#include <system_error>
#include <utility>

#include <openssl/err.h>

#include "fiber/RequestContext.hpp"

namespace sylar
{
namespace
{

std::runtime_error tlsError(const char* operation)
{
    char buffer[256]{};
    const auto code = ::ERR_get_error();
    if (code != 0)
    {
        ::ERR_error_string_n(code, buffer, sizeof(buffer));
    }
    return std::runtime_error(
        std::string(operation) + ": " +
        (code == 0 ? "TLS failure" : buffer)
    );
}

SSL_CTX* makeContext(const SSL_METHOD* method)
{
    SSL_CTX* context = ::SSL_CTX_new(method);
    if (!context)
    {
        throw tlsError("SSL_CTX_new");
    }
    if (::SSL_CTX_set_min_proto_version(context, TLS1_2_VERSION) != 1)
    {
        ::SSL_CTX_free(context);
        throw tlsError("TLS minimum version");
    }
    return context;
}

bool isNumericAddress(const std::string& host)
{
    in_addr v4{};
    in6_addr v6{};
    return ::inet_pton(AF_INET, host.c_str(), &v4) == 1 ||
           ::inet_pton(AF_INET6, host.c_str(), &v6) == 1;
}

} // namespace

TlsContext::TlsContext(SSL_CTX* context, bool client) noexcept
    : m_context(context)
    , m_client(client)
{
}

std::shared_ptr<TlsContext> TlsContext::server(
    const std::string& certificate, const std::string& private_key
)
{
    SSL_CTX* context = makeContext(::TLS_server_method());
    if (::SSL_CTX_use_certificate_chain_file(
            context,
            certificate.c_str()
        ) != 1 ||
        ::SSL_CTX_use_PrivateKey_file(
            context,
            private_key.c_str(),
            SSL_FILETYPE_PEM
        ) != 1 ||
        ::SSL_CTX_check_private_key(context) != 1)
    {
        ::SSL_CTX_free(context);
        throw tlsError("TLS server credentials");
    }
    return std::shared_ptr<TlsContext>(new TlsContext(context, false)
    );
}

std::shared_ptr<TlsContext>
TlsContext::client(const std::string& ca_file)
{
    SSL_CTX* context = makeContext(::TLS_client_method());
    ::SSL_CTX_set_verify(context, SSL_VERIFY_PEER, nullptr);
    const bool loaded = ca_file.empty()
                            ? ::SSL_CTX_set_default_verify_paths(
                                  context
                              ) == 1
                            : ::SSL_CTX_load_verify_locations(
                                  context,
                                  ca_file.c_str(),
                                  nullptr
                              ) == 1;
    if (!loaded)
    {
        ::SSL_CTX_free(context);
        throw tlsError("TLS trust store");
    }
    return std::shared_ptr<TlsContext>(new TlsContext(context, true));
}

TlsContext::~TlsContext()
{
    ::SSL_CTX_free(m_context);
}

SSL_CTX* TlsContext::native() const noexcept
{
    return m_context;
}

bool TlsContext::clientMode() const noexcept
{
    return m_client;
}

TlsStream::TlsStream(
    IOManager& io,
    Socket socket,
    std::shared_ptr<TlsContext> context,
    Role role,
    std::string peer_host,
    std::chrono::milliseconds timeout
)
    : m_io(io)
    , m_socket(std::move(socket))
    , m_context(std::move(context))
    , m_timeout(timeout)
{
    if (!m_context || timeout.count() < 0 ||
        m_context->clientMode() != (role == Role::Client))
    {
        throw std::invalid_argument("invalid TLS stream options");
    }
    m_ssl = ::SSL_new(m_context->native());
    if (!m_ssl)
    {
        throw tlsError("SSL_new");
    }
    try
    {
        if (::SSL_set_fd(m_ssl, m_socket.fd()) != 1)
        {
            throw tlsError("SSL_set_fd");
        }
        if (role == Role::Client)
        {
            if (peer_host.empty() ||
                ::SSL_set1_host(m_ssl, peer_host.c_str()) != 1)
            {
                throw tlsError("TLS peer name");
            }
            if (!isNumericAddress(peer_host) &&
                ::SSL_set_tlsext_host_name(
                    m_ssl,
                    peer_host.c_str()
                ) != 1)
            {
                throw tlsError("TLS SNI");
            }
            ::SSL_set_connect_state(m_ssl);
        }
        else
        {
            ::SSL_set_accept_state(m_ssl);
        }
    }
    catch (...)
    {
        ::SSL_free(m_ssl);
        m_ssl = nullptr;
        throw;
    }
}

TlsStream::~TlsStream()
{
    if (m_ssl)
    {
        ::SSL_shutdown(m_ssl);
        ::SSL_free(m_ssl);
    }
}

void TlsStream::checkRequestContext() const
{
    if (m_requestContext && (m_requestContext->cancelled() ||
                             m_requestContext->expired()))
    {
        throw std::system_error(
            m_requestContext->cancelled() ? ECANCELED : ETIMEDOUT,
            std::generic_category(),
            "TLS request"
        );
    }
}

void TlsStream::waitFor(IOManager::Event event)
{
    const auto result = event == IOManager::Event::Read
                            ? (m_requestContext
                                   ? m_io.waitReadable(
                                         m_socket.fd(),
                                         m_timeout,
                                         *m_requestContext
                                     )
                                   : m_io.waitReadable(
                                         m_socket.fd(),
                                         m_timeout
                                     ))
                            : (m_requestContext
                                   ? m_io.waitWritable(
                                         m_socket.fd(),
                                         m_timeout,
                                         *m_requestContext
                                     )
                                   : m_io.waitWritable(
                                         m_socket.fd(),
                                         m_timeout
                                     ));
    if (result == IOManager::WaitResult::TimedOut)
    {
        throw std::system_error(
            ETIMEDOUT,
            std::generic_category(),
            "TLS wait"
        );
    }
    if (result == IOManager::WaitResult::Cancelled)
    {
        throw std::system_error(
            ECANCELED,
            std::generic_category(),
            "TLS wait"
        );
    }
}

int TlsStream::handleRetry(int result)
{
    const int error = ::SSL_get_error(m_ssl, result);
    if (error == SSL_ERROR_WANT_READ)
    {
        waitFor(IOManager::Event::Read);
        return error;
    }
    if (error == SSL_ERROR_WANT_WRITE)
    {
        waitFor(IOManager::Event::Write);
        return error;
    }
    if (error == SSL_ERROR_ZERO_RETURN)
    {
        return error;
    }
    if (error == SSL_ERROR_SYSCALL && errno != 0)
    {
        throw std::system_error(
            errno,
            std::generic_category(),
            "TLS I/O"
        );
    }
    throw tlsError("TLS I/O");
}

void TlsStream::handshake()
{
    if (m_handshaken)
    {
        return;
    }
    for (;;)
    {
        checkRequestContext();
        const int result = ::SSL_do_handshake(m_ssl);
        if (result == 1)
        {
            m_handshaken = true;
            return;
        }
        if (handleRetry(result) == SSL_ERROR_ZERO_RETURN)
        {
            throw std::runtime_error("TLS handshake closed");
        }
    }
}

std::size_t TlsStream::readSome(std::span<std::byte> buffer)
{
    if (buffer.empty())
    {
        return 0;
    }
    handshake();
    for (;;)
    {
        checkRequestContext();
        std::size_t count = 0;
        const int result = ::SSL_read_ex(
            m_ssl,
            buffer.data(),
            buffer.size(),
            &count
        );
        if (result == 1)
        {
            return count;
        }
        if (handleRetry(result) == SSL_ERROR_ZERO_RETURN)
        {
            return 0;
        }
    }
}

std::size_t TlsStream::writeSome(std::span<const std::byte> buffer)
{
    if (buffer.empty())
    {
        return 0;
    }
    handshake();
    for (;;)
    {
        checkRequestContext();
        std::size_t count = 0;
        const int result = ::SSL_write_ex(
            m_ssl,
            buffer.data(),
            buffer.size(),
            &count
        );
        if (result == 1)
        {
            return count;
        }
        if (handleRetry(result) == SSL_ERROR_ZERO_RETURN)
        {
            throw std::runtime_error("TLS connection closed");
        }
    }
}

Socket& TlsStream::socket() noexcept
{
    return m_socket;
}

} // namespace sylar
