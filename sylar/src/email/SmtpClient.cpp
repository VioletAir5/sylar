#include "email/SmtpClient.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <memory>
#include <span>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

#include "net/Address.hpp"
#include "net/Socket.hpp"
#include "net/SocketStream.hpp"

namespace sylar
{
namespace
{

constexpr char kCrLf[] = {13, 10, 0};

void validateAddress(std::string_view address)
{
    if (address.empty())
    {
        throw std::invalid_argument("empty SMTP envelope address");
    }
    for (const unsigned char ch : address)
    {
        if (ch <= 32 || ch >= 127 || ch == '<' || ch == '>')
        {
            throw std::invalid_argument(
                "invalid SMTP envelope address"
            );
        }
    }
}

void write(Stream& stream, std::string_view value)
{
    stream.writeAll(std::as_bytes(std::span(value)));
}

class ReplyReader
{
public:
    explicit ReplyReader(Stream& stream)
        : m_stream(stream)
    {
    }

    int reply(std::vector<std::string>* lines = nullptr)
    {
        int code = 0;
        for (int count = 0; count < 32; ++count)
        {
            const auto response = line();
            if (lines)
            {
                lines->push_back(response);
            }
            if (response.size() < 4 ||
                !std::isdigit(static_cast<unsigned char>(response[0])
                ) ||
                !std::isdigit(static_cast<unsigned char>(response[1])
                ) ||
                !std::isdigit(static_cast<unsigned char>(response[2])
                ) ||
                (response[3] != '-' && response[3] != ' '))
            {
                throw std::runtime_error("malformed SMTP reply");
            }
            const int current = (response[0] - '0') * 100 +
                                (response[1] - '0') * 10 +
                                response[2] - '0';
            if (current != code && code != 0)
            {
                throw std::runtime_error(
                    "inconsistent SMTP reply code"
                );
            }
            code = current;
            if (response[3] == ' ')
            {
                return code;
            }
        }
        throw std::runtime_error("SMTP reply has too many lines");
    }

    bool hasBufferedData() const noexcept
    {
        return !m_buffer.empty();
    }

private:
    std::string line()
    {
        for (;;)
        {
            const auto end = m_buffer.find(kCrLf);
            if (end != std::string::npos)
            {
                if (end > 2048)
                {
                    throw std::runtime_error(
                        "SMTP reply line too long"
                    );
                }
                auto result = m_buffer.substr(0, end);
                m_buffer.erase(0, end + 2);
                return result;
            }
            if (m_buffer.size() > 2048)
            {
                throw std::runtime_error("SMTP reply line too long");
            }
            std::array<std::byte, 1024> bytes{};
            const auto count = m_stream.readSome(bytes);
            if (count == 0)
            {
                throw std::runtime_error("SMTP server closed early");
            }
            m_buffer.append(
                reinterpret_cast<const char*>(bytes.data()),
                count
            );
        }
    }

    Stream& m_stream;
    std::string m_buffer;
};

void expect(ReplyReader& reader, int expected, const char* step)
{
    const auto actual = reader.reply();
    if (actual != expected)
    {
        throw std::runtime_error(
            std::string(step) + ": SMTP reply " +
            std::to_string(actual)
        );
    }
}

bool advertisesStartTls(const std::vector<std::string>& lines)
{
    for (const auto& line : lines)
    {
        if (line.size() >= 12 && line.substr(0, 4) == "250-")
        {
            std::string extension = line.substr(4);
            std::transform(
                extension.begin(),
                extension.end(),
                extension.begin(),
                [](unsigned char ch)
                { return static_cast<char>(std::toupper(ch)); }
            );
            if (extension == "STARTTLS")
            {
                return true;
            }
        }
    }
    return false;
}

std::string encodeData(std::string_view data)
{
    if (data.empty())
    {
        throw std::invalid_argument("empty SMTP message");
    }
    std::string result;
    result.reserve(data.size() + 64);
    std::size_t start = 0;
    while (start < data.size())
    {
        const auto end = data.find(char(10), start);
        auto line = data.substr(
            start,
            end == std::string_view::npos ? data.size() - start
                                          : end - start
        );
        if (!line.empty() && line.back() == char(13))
        {
            line.remove_suffix(1);
        }
        if (line.find(char(13)) != std::string_view::npos)
        {
            throw std::invalid_argument("bare CR in SMTP message");
        }
        const bool dot = !line.empty() && line.front() == '.';
        if (line.size() > static_cast<std::size_t>(dot ? 997 : 998))
        {
            throw std::invalid_argument("SMTP message line too long");
        }
        if (dot)
        {
            result.push_back('.');
        }
        result += line;
        result += kCrLf;
        if (end == std::string_view::npos)
        {
            break;
        }
        start = end + 1;
    }
    result.push_back('.');
    result += kCrLf;
    return result;
}

} // namespace

void SmtpClient::send(
    IOManager& io,
    const std::string& host,
    std::uint16_t port,
    const SmtpMessage& message,
    std::string helo_host,
    std::chrono::milliseconds timeout,
    std::shared_ptr<TlsContext> tls,
    bool start_tls
)
{
    if (host.empty() || port == 0 || helo_host.empty() ||
        helo_host.find_first_of(" \t\r\n") != std::string::npos ||
        message.recipients.empty() || (start_tls && !tls))
    {
        throw std::invalid_argument(
            "invalid SMTP connection or recipients"
        );
    }
    validateAddress(message.from);
    for (const auto& recipient : message.recipients)
    {
        validateAddress(recipient);
    }
    const auto encoded = encodeData(message.data);
    const auto addresses = Address::resolve(
        host,
        std::to_string(port)
    );
    if (addresses.empty())
    {
        throw std::runtime_error("SMTP host has no addresses");
    }
    auto socket = Socket::tcp(addresses.front().family());
    socket.connect(io, addresses.front(), timeout);
    std::unique_ptr<Stream> stream;
    SocketStream* plain_stream = nullptr;
    if (tls && !start_tls)
    {
        stream = std::make_unique<TlsStream>(
            io,
            std::move(socket),
            std::move(tls),
            TlsStream::Role::Client,
            host,
            timeout
        );
    }
    else
    {
        auto plain = std::make_unique<SocketStream>(
            io,
            std::move(socket),
            timeout
        );
        plain_stream = plain.get();
        stream = std::move(plain);
    }
    auto replies = std::make_unique<ReplyReader>(*stream);
    expect(*replies, 220, "greeting");
    write(*stream, "EHLO " + helo_host + kCrLf);
    if (start_tls)
    {
        std::vector<std::string> capabilities;
        if (replies->reply(&capabilities) != 250 ||
            !advertisesStartTls(capabilities))
        {
            throw std::runtime_error("SMTP STARTTLS not advertised");
        }
        write(*stream, std::string("STARTTLS") + kCrLf);
        expect(*replies, 220, "STARTTLS");
        if (replies->hasBufferedData())
        {
            throw std::runtime_error(
                "unexpected data before SMTP TLS upgrade"
            );
        }
        replies.reset();
        auto upgraded_socket = std::move(plain_stream->socket());
        stream.reset();
        stream = std::make_unique<TlsStream>(
            io,
            std::move(upgraded_socket),
            std::move(tls),
            TlsStream::Role::Client,
            host,
            timeout
        );
        replies = std::make_unique<ReplyReader>(*stream);
        write(*stream, "EHLO " + helo_host + kCrLf);
        expect(*replies, 250, "EHLO after STARTTLS");
    }
    else
    {
        expect(*replies, 250, "EHLO");
    }
    write(*stream, "MAIL FROM:<" + message.from + ">" + kCrLf);
    expect(*replies, 250, "MAIL FROM");
    for (const auto& recipient : message.recipients)
    {
        write(*stream, "RCPT TO:<" + recipient + ">" + kCrLf);
        const auto code = replies->reply();
        if (code != 250 && code != 251)
        {
            throw std::runtime_error(
                "RCPT TO: SMTP reply " + std::to_string(code)
            );
        }
    }
    write(*stream, std::string("DATA") + kCrLf);
    expect(*replies, 354, "DATA");
    write(*stream, encoded);
    expect(*replies, 250, "message body");
    write(*stream, std::string("QUIT") + kCrLf);
    expect(*replies, 221, "QUIT");
}

} // namespace sylar