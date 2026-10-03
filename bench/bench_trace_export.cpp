#include <algorithm>
#include <atomic>
#include <chrono>
#include <iostream>
#include <thread>
#include <vector>

#include <fmt/format.h>

#include "rpc/Trace.hpp"
using namespace std::chrono_literals;
int main()
{
    sylar::rpc::TraceRecorder recorder(4096);
    auto makeSpan = []
    {
        return sylar::rpc::SpanRecord{
            "0123456789abcdef0123456789abcdef",
            "0123456789abcdef",
            "fedcba9876543210",
            "Catalog.GetProduct",
            "rpc.server",
            1,
            10,
            0,
            0};
    };
    for (unsigned i = 0; i < 4096; ++i)
        recorder.record(makeSpan());
    std::atomic_bool finished{false}, started{false};
    std::atomic_uint64_t exports{0};
    std::jthread reader(
        [&]
        {
            started.store(true);
            while (!finished.load())
            {
                auto json = recorder.json();
                if (json.empty())
                    std::terminate();
                ++exports;
                std::this_thread::sleep_for(100us);
            }
        }
    );
    while (!started.load())
        std::this_thread::yield();
    std::vector<double> samples;
    for (unsigned i = 0; i < 1000; ++i)
    {
        auto span = makeSpan();
        auto start = std::chrono::steady_clock::now();
        recorder.record(std::move(span));
        samples.push_back(std::chrono::duration<double, std::micro>(
                              std::chrono::steady_clock::now() - start
        )
                              .count());
        std::this_thread::sleep_for(100us);
    }
    finished.store(true);
    reader.join();
    std::sort(samples.begin(), samples.end());
    std::cout << fmt::format(
        "{{\"records\":1000,\"capacity\":4096,\"exports\":{},"
        "\"record_p50_us\":{},\"record_p95_us\":{},\"record_p99_us\":"
        "{},"
        "\"record_max_us\":{},\"retained\":{},\"dropped\":{}}}\n",
        exports.load(),
        samples[500],
        samples[950],
        samples[990],
        samples.back(),
        recorder.size(),
        recorder.dropped()
    );
    return recorder.size() == 4096 && recorder.dropped() == 1000 ? 0
                                                                 : 1;
}
