#include <atomic>
#include <charconv>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <new>
#include <string>

#include <fmt/format.h>

#include "rpc/Trace.hpp"

namespace
{
thread_local bool counting = false;
thread_local std::uint64_t allocations = 0;
} // namespace
void* operator new(std::size_t size)
{
    if (auto* result = std::malloc(size ? size : 1))
    {
        if (counting)
            ++allocations;
        return result;
    }
    throw std::bad_alloc();
}
void operator delete(void* pointer) noexcept
{
    std::free(pointer);
}
void operator delete(void* pointer, std::size_t) noexcept
{
    std::free(pointer);
}

int main(int argc, char** argv)
{
    try
    {
        std::uint64_t count = 1000000;
        if (argc > 3)
            throw std::invalid_argument(
                "usage: sylar_trace_bench [iterations] "
                "[max-allocations-per-header]"
            );
        if (argc > 1)
        {
            std::string_view value(argv[1]);
            auto [end, error] = std::from_chars(
                value.data(),
                value.data() + value.size(),
                count
            );
            if (error != std::errc{} ||
                end != value.data() + value.size() || count == 0 ||
                count > 100000000)
                throw std::invalid_argument("invalid iteration count"
                );
        }
        sylar::rpc::TraceContext context{
            "0123456789abcdef0123456789abcdef",
            "0123456789abcdef",
            "01"};
        const std::string expected =
            "00-0123456789abcdef0123456789abcdef-0123456789abcdef-01";
        if (context.header() != expected)
            throw std::runtime_error("traceparent encoding changed");
        std::uint64_t checksum = 0;
        for (unsigned i = 0; i < 10000; ++i)
            checksum += context.header().size();
        allocations = 0;
        counting = true;
        for (unsigned i = 0; i < 10000; ++i)
            checksum += context.header().size();
        counting = false;
        const auto allocation_count = allocations;
        const auto started = std::chrono::steady_clock::now();
        for (std::uint64_t i = 0; i < count; ++i)
        {
            auto header = context.header();
            checksum += header.size() +
                        static_cast<unsigned char>(header[3]);
        }
        const double seconds = std::chrono::duration<double>(
                                   std::chrono::steady_clock::now() -
                                   started
        )
                                   .count();
        const double per_call = allocation_count / 10000.0;
        std::cout << fmt::format(
            "{{\"iterations\":{},\"seconds\":{},\"ns_per_header\":{},"
            "\"headers_per_second\":{},\"allocations_per_header\":{},"
            "\"checksum\":{}}}\n",
            count,
            seconds,
            seconds * 1e9 / count,
            count / seconds,
            per_call,
            checksum
        );
        if (argc > 2)
        {
            unsigned maximum = 0;
            std::string_view value(argv[2]);
            auto [end, error] = std::from_chars(
                value.data(),
                value.data() + value.size(),
                maximum
            );
            if (error != std::errc{} ||
                end != value.data() + value.size())
                throw std::invalid_argument("invalid allocation limit"
                );
            if (per_call > maximum)
                return 2;
        }
        return 0;
    }
    catch (const std::exception& error)
    {
        counting = false;
        std::cerr << error.what() << '\n';
        return 1;
    }
}
