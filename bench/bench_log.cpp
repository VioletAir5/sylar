// Scenario selection follows spdlog/bench/bench.cpp (MIT):
// https://github.com/gabime/spdlog/blob/v1.x/bench/bench.cpp
// This implementation benchmarks sylar's synchronous logger, not
// spdlog itself.

#include <charconv>
#include <chrono>
#include <cstddef>
#include <exception>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <fmt/format.h>

#include "log/FileSink.hpp"
#include "log/Logger.hpp"

namespace
{

using Clock = std::chrono::steady_clock;

struct Timings
{
    double loggingSeconds;
    double flushSeconds;
};

template <typename Action>
Timings measure(Action&& action, sylar::log::Logger& logger)
{
    const auto start = Clock::now();
    action();
    const auto logged = Clock::now();
    logger.flush();
    const auto flushed = Clock::now();

    return {
        std::chrono::duration<double>(logged - start).count(),
        std::chrono::duration<double>(flushed - logged).count()};
}

void printResult(
    std::string_view label, std::size_t count, Timings timings
)
{
    const double messagesPerSecond = timings.loggingSeconds > 0
                                         ? count /
                                               timings.loggingSeconds
                                         : 0.0;
    fmt::print(
        "{:<20} {:>10.6f} s {:>14.0f} msg/s  flush {:>8.3f} ms\n",
        label,
        timings.loggingSeconds,
        messagesPerSecond,
        timings.flushSeconds * 1000.0
    );
}

void logSingle(sylar::log::Logger& logger, std::size_t count)
{
    for (std::size_t index = 0; index < count; ++index)
    {
        SYLAR_LOG_INFO(logger, "message {}", index);
    }
}

void logThreaded(
    sylar::log::Logger& logger,
    std::size_t count,
    std::size_t threadCount
)
{
    std::vector<std::thread> workers;
    std::vector<std::exception_ptr> errors(threadCount);
    workers.reserve(threadCount);

    for (std::size_t worker = 0; worker < threadCount; ++worker)
    {
        workers.emplace_back(
            [&logger, &errors, count, threadCount, worker]
            {
                try
                {
                    for (std::size_t index = worker; index < count;
                         index += threadCount)
                    {
                        SYLAR_LOG_INFO(logger, "message {}", index);
                    }
                }
                catch (...)
                {
                    errors[worker] = std::current_exception();
                }
            }
        );
    }

    for (auto& worker : workers)
    {
        worker.join();
    }
    for (const auto& error : errors)
    {
        if (error)
        {
            std::rethrow_exception(error);
        }
    }
}

void runFileCase(
    std::string_view label,
    std::size_t count,
    std::size_t threadCount,
    const std::filesystem::path& path,
    bool launchWorkers
)
{
    auto sink = std::make_shared<sylar::log::FileSink>(path);
    sylar::log::Logger logger("bench", {sink});

    auto timings = measure(
        [&]
        {
            if (launchWorkers)
            {
                logThreaded(logger, count, threadCount);
            }
            else
            {
                logSingle(logger, count);
            }
        },
        logger
    );
    printResult(label, count, timings);
}

void runDisabledCase(
    std::string_view label,
    std::size_t count,
    std::size_t threadCount,
    bool launchWorkers
)
{
    sylar::log::Logger logger(
        "disabled",
        {},
        sylar::log::LogLevel::OFF
    );

    auto timings = measure(
        [&]
        {
            if (launchWorkers)
            {
                logThreaded(logger, count, threadCount);
            }
            else
            {
                logSingle(logger, count);
            }
        },
        logger
    );
    printResult(label, count, timings);
}

std::size_t positiveCount(const char* value, std::string_view name)
{
    const std::string_view input(value);
    std::size_t parsed = 0;
    const auto [end, error] = std::from_chars(
        input.data(),
        input.data() + input.size(),
        parsed
    );
    if (error != std::errc{} || end != input.data() + input.size() ||
        parsed == 0)
    {
        throw std::invalid_argument(
            fmt::format("{} must be a positive integer", name)
        );
    }
    return parsed;
}

} // namespace

int main(int argc, char* argv[])
{
    try
    {
        const std::size_t count = argc > 1 ? positiveCount(
                                                 argv[1],
                                                 "count"
                                             )
                                           : 250000;
        const std::size_t threads = argc > 2 ? positiveCount(
                                                   argv[2],
                                                   "threads"
                                               )
                                             : 4;
        if (threads > 256)
        {
            throw std::invalid_argument("threads must be at most 256"
            );
        }

        const auto defaultDirectory =
            std::filesystem::temp_directory_path() /
            fmt::format(
                "sylar-bench-{}",
                Clock::now().time_since_epoch().count()
            );
        const auto directory = argc > 3
                                   ? std::filesystem::path(argv[3])
                                   : defaultDirectory;
        std::filesystem::create_directories(directory);

        fmt::print("sylar synchronous logging benchmark\n");
        fmt::print(
            "messages: {}, threads: {}, log files: {}\n",
            count,
            threads,
            directory.string()
        );
        fmt::print("logging time includes thread creation for "
                   "threaded cases; flush is reported separately\n\n"
        );

        runFileCase(
            "file/single",
            count,
            1,
            directory / "single.log",
            false
        );
        runFileCase(
            "file/1-thread",
            count,
            1,
            directory / "one-thread.log",
            true
        );
        runFileCase(
            "file/N-thread",
            count,
            threads,
            directory / "multi-thread.log",
            true
        );
        runDisabledCase("level-off/single", count, 1, false);
        runDisabledCase("level-off/N-thread", count, threads, true);
        return 0;
    }
    catch (const std::exception& error)
    {
        fmt::print(stderr, "benchmark failed: {}\n", error.what());
        return 1;
    }
}
