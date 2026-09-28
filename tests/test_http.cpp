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
                         "Transfer-Encoding: chunked\r\n\r\n");
    sylar::HttpSession third(chunked);
    EXPECT_THROW(third.readRequest(request), std::invalid_argument);
}
