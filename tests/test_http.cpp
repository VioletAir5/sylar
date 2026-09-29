#include <algorithm>
#include <cstddef>
#include <cstring>
#include <span>
#include <string>

#include <gtest/gtest.h>

#include "http/Http.hpp"

namespace
{

class MemoryStream final : public sylar::Stream
{
public:
    explicit MemoryStream(std::string input, std::size_t chunk = 7)
        : m_input(std::move(input))
        , m_chunk(chunk)
    {
    }

    std::size_t readSome(std::span<std::byte> output) override
    {
        const auto count = std::min(
            {output.size(), m_chunk, m_input.size() - m_position}
        );
        if (count != 0)
        {
            std::memcpy(
                output.data(),
                m_input.data() + m_position,
                count
            );
            m_position += count;
        }
        return count;
    }

    std::size_t writeSome(std::span<const std::byte> input) override
    {
        const auto count = std::min(input.size(), m_chunk);
        m_output.append(
            reinterpret_cast<const char*>(input.data()),
            count
        );
        return count;
    }

    std::string output() const
    {
        return m_output;
    }

private:
    std::string m_input;
    std::string m_output;
    std::size_t m_chunk;
    std::size_t m_position = 0;
};

} // namespace

TEST(HttpTest, ParsesPipelinedRequestsAndWritesResponse)
{
    MemoryStream stream("POST /echo HTTP/1.1\r\nHost: test\r\n"
                        "Content-Length: 5\r\n\r\nhello"
                        "GET /next HTTP/1.1\r\nHost: test\r\n"
                        "Connection: close\r\n\r\n");
    sylar::HttpSession session(stream);
    sylar::HttpRequest request;
    ASSERT_TRUE(session.readRequest(request));
    EXPECT_EQ(request.method, "POST");
    EXPECT_EQ(request.target, "/echo");
    EXPECT_EQ(request.body, "hello");
    EXPECT_TRUE(request.keepAlive());
    ASSERT_TRUE(session.readRequest(request));
    EXPECT_EQ(request.target, "/next");
    EXPECT_FALSE(request.keepAlive());
    EXPECT_FALSE(session.readRequest(request));

    sylar::HttpResponse response;
    response.body = "done";
    session.sendResponse(response, false);
    EXPECT_NE(
        stream.output().find("Content-Length: 4\r\n"),
        std::string::npos
    );
    EXPECT_NE(
        stream.output().find("Connection: close\r\n"),
        std::string::npos
    );
}

TEST(HttpTest, RejectsAmbiguousFramingAndOversizedBody)
{
    sylar::HttpRequest request;
    MemoryStream duplicate(
        "POST / HTTP/1.1\r\nHost: x\r\n"
        "Content-Length: 1\r\nContent-Length: 1\r\n\r\nx"
    );
    sylar::HttpSession first(duplicate);
    EXPECT_THROW(first.readRequest(request), std::invalid_argument);
    MemoryStream oversized("POST / HTTP/1.1\r\nHost: x\r\n"
                           "Content-Length: 9\r\n\r\n123456789");
    sylar::HttpSession second(oversized, 100, 8);
    EXPECT_THROW(second.readRequest(request), std::invalid_argument);
    MemoryStream chunked("POST / HTTP/1.1\r\nHost: x\r\n"
                         "Transfer-Encoding: chunked\r\n"
                         "Content-Length: 1\r\n\r\nx");
    sylar::HttpSession third(chunked);
    EXPECT_THROW(third.readRequest(request), std::invalid_argument);
}

TEST(HttpTest, ParsesChunkedBodyAndNextRequest)
{
    MemoryStream stream("POST /chunks HTTP/1.1\r\nHost: test\r\n"
                        "Transfer-Encoding: chunked\r\n\r\n"
                        "5\r\nhello\r\n"
                        "6\r\n world\r\n"
                        "0\r\nX-Trailer: ok\r\n\r\n"
                        "GET /next HTTP/1.1\r\nHost: test\r\n\r\n");
    sylar::HttpSession session(stream);
    sylar::HttpRequest request;
    ASSERT_TRUE(session.readRequest(request));
    EXPECT_EQ(request.body, "hello world");
    ASSERT_TRUE(session.readRequest(request));
    EXPECT_EQ(request.target, "/next");
}

TEST(HttpTest, ConnectionTokensAndEmptyHost)
{
    MemoryStream stream(
        "GET / HTTP/1.1\r\nHost: example\r\n"
        "Connection: keep-alive, CLOSE\r\n\r\n"
        "GET / HTTP/1.0\r\nConnection: Upgrade, Keep-Alive\r\n\r\n"
    );
    sylar::HttpSession session(stream);
    sylar::HttpRequest request;
    ASSERT_TRUE(session.readRequest(request));
    EXPECT_FALSE(request.keepAlive());
    ASSERT_TRUE(session.readRequest(request));
    EXPECT_TRUE(request.keepAlive());

    MemoryStream bad("GET / HTTP/1.1\r\nHost: \r\n\r\n");
    sylar::HttpSession bad_session(bad);
    EXPECT_THROW(
        bad_session.readRequest(request),
        std::invalid_argument
    );
}

TEST(HttpTest, HeadAndNoContentResponsesHaveNoBody)
{
    sylar::HttpResponse response;
    response.body = "payload";
    const auto head = response.serialize(true, true);
    EXPECT_NE(head.find("Content-Length: 7\r\n"), std::string::npos);
    EXPECT_EQ(head.substr(head.size() - 4), "\r\n\r\n");
    EXPECT_EQ(head.find("payload"), std::string::npos);

    response.status = 204;
    response.reason = "No Content";
    const auto no_content = response.serialize();
    EXPECT_EQ(no_content.find("Content-Length:"), std::string::npos);
    EXPECT_EQ(no_content.find("payload"), std::string::npos);
    EXPECT_EQ(no_content.substr(no_content.size() - 4), "\r\n\r\n");

    response.status = 304;
    response.reason = "Not Modified";
    const auto not_modified = response.serialize();
    EXPECT_EQ(
        not_modified.find("Content-Length:"),
        std::string::npos
    );
    EXPECT_EQ(not_modified.find("payload"), std::string::npos);
}

TEST(HttpTest, RejectsNonAsciiHeaderNames)
{
    sylar::HttpRequest request;
    MemoryStream stream("GET / HTTP/1.1\r\nHost: example\r\n"
                        "X-\xC3\xA9: value\r\n\r\n");
    sylar::HttpSession session(stream);
    EXPECT_THROW(session.readRequest(request), std::invalid_argument);
}

TEST(HttpTest, RejectsControlBytesInTargetAndFields)
{
    sylar::HttpRequest request;
    MemoryStream target("GET /\x01 HTTP/1.1\r\nHost: example\r\n\r\n"
    );
    sylar::HttpSession target_session(target);
    EXPECT_THROW(
        target_session.readRequest(request),
        std::invalid_argument
    );

    MemoryStream field(
        std::string("GET / HTTP/1.1\r\nHost: example") + '\0' +
        "\r\n\r\n"
    );
    sylar::HttpSession field_session(field);
    EXPECT_THROW(
        field_session.readRequest(request),
        std::invalid_argument
    );

    sylar::HttpResponse response;
    response.headers["X-Test"] = std::string("bad") + '\0';
    EXPECT_THROW(response.serialize(), std::invalid_argument);
}
