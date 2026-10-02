#include <algorithm>
#include <cstddef>
#include <cstring>
#include <deque>
#include <span>
#include <stdexcept>
#include <string>

#include <gtest/gtest.h>

#include "http/Http.hpp"

namespace
{

class FaultStream final : public sylar::Stream
{
public:
    explicit FaultStream(
        std::string input, std::size_t read_chunk = 1
    )
        : input(std::move(input))
        , read_chunk(read_chunk)
    {
    }

    std::size_t readSome(std::span<std::byte> buffer) override
    {
        if (read_error_at == read_calls++)
        {
            throw std::runtime_error("injected read failure");
        }
        const auto count = std::min(
            {buffer.size(), read_chunk, input.size() - position}
        );
        if (count)
        {
            std::memcpy(
                buffer.data(),
                input.data() + position,
                count
            );
            position += count;
        }
        return count;
    }

    std::size_t writeSome(std::span<const std::byte> buffer) override
    {
        if (write_error_at == write_calls++)
        {
            throw std::runtime_error("injected write failure");
        }
        const auto count = std::min(buffer.size(), write_chunk);
        output.append(
            reinterpret_cast<const char*>(buffer.data()),
            count
        );
        return count;
    }

    std::string input;
    std::string output;
    std::size_t read_chunk;
    std::size_t write_chunk = 2;
    std::size_t position = 0;
    std::size_t read_calls = 0;
    std::size_t write_calls = 0;
    std::size_t read_error_at = static_cast<std::size_t>(-1);
    std::size_t write_error_at = static_cast<std::size_t>(-1);
};

} // namespace

TEST(FaultStreamTest, FragmentedRequestAndHalfCloseRemainParseable)
{
    FaultStream stream(
        "POST /echo HTTP/1.1\r\nHost: local\r\n"
        "Content-Length: 5\r\n\r\nhello",
        1
    );
    sylar::HttpSession session(stream);
    sylar::HttpRequest request;
    ASSERT_TRUE(session.readRequest(request));
    EXPECT_EQ(request.body, "hello");
    EXPECT_FALSE(session.readRequest(request));
    sylar::HttpResponse response;
    response.body = "ok";
    session.sendResponse(response, false);
    EXPECT_NE(
        stream.output.find("HTTP/1.1 200 OK"),
        std::string::npos
    );
    EXPECT_TRUE(stream.output.ends_with("\r\n\r\nok"));
    EXPECT_GT(stream.write_calls, 5);
}

TEST(FaultStreamTest, TruncationAndInjectedWriteFailFast)
{
    FaultStream truncated("POST /echo HTTP/1.1\r\nHost: local\r\n"
                          "Content-Length: 5\r\n\r\nhe");
    sylar::HttpSession first(truncated);
    sylar::HttpRequest request;
    EXPECT_THROW(first.readRequest(request), std::invalid_argument);

    FaultStream failed("");
    failed.write_error_at = 2;
    sylar::HttpSession second(failed);
    sylar::HttpResponse response;
    response.body = "ok";
    EXPECT_THROW(
        second.sendResponse(response, false),
        std::runtime_error
    );
    EXPECT_EQ(failed.write_calls, 3);
}
