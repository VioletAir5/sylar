#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "log/FileSink.hpp"
#include "log/Level.hpp"
#include "log/Logger.hpp"
#include "log/OstreamSink.hpp"
#include "log/PatternFormatter.hpp"

namespace
{

std::string readFile(const std::filesystem::path& path)
{
    std::ifstream input(path);
    return {
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>()};
}

TEST(PatternFormatterTest, FormatsFieldsAndRejectsBadPatterns)
{
    std::ostringstream output;
    auto sink = std::make_shared<sylar::log::OstreamSink>(output);
    sink->setPattern("%n|%l|%v|%s:%#|%%");
    sylar::log::Logger logger("io", {sink});

    logger.warn("closed");
    EXPECT_NE(
        output.str().find("io|WARN|closed|test_sinks.cpp:"),
        std::string::npos
    );
    EXPECT_TRUE(output.str().ends_with("|%\n"));

    EXPECT_THROW(sink->setPattern("%q"), std::invalid_argument);
    EXPECT_THROW(
        sink->setPattern("trailing%"),
        std::invalid_argument
    );
    logger.info("still works");
    EXPECT_NE(
        output.str().find("io|INFO|still works"),
        std::string::npos
    );
}

TEST(PatternFormatterTest, LoggerPatternAppliesToAllSinks)
{
    std::ostringstream firstOutput;
    std::ostringstream secondOutput;
    auto first = std::make_shared<sylar::log::OstreamSink>(firstOutput
    );
    auto second = std::make_shared<sylar::log::OstreamSink>(
        secondOutput
    );
    sylar::log::Logger logger("core", {first, second});

    logger.setPattern("%l:%v");
    logger.error("failed");

    EXPECT_EQ(firstOutput.str(), "ERROR:failed\n");
    EXPECT_EQ(secondOutput.str(), "ERROR:failed\n");
    EXPECT_THROW(logger.setPattern("%z"), std::invalid_argument);
}

TEST(FileSinkTest, AppendsAndReopensAfterRotation)
{
    const auto stamp = std::chrono::steady_clock::now()
                           .time_since_epoch()
                           .count();
    const auto directory = std::filesystem::temp_directory_path() /
                           ("sylar-log-test-" + std::to_string(stamp)
                           );
    const auto path = directory / "server.log";
    const auto moved = directory / "server.log.old";

    {
        auto sink = std::make_shared<sylar::log::FileSink>(path);
        sink->setPattern("%v");
        sylar::log::Logger logger("server", {sink});
        logger.info("before");
        logger.flush();
        EXPECT_EQ(readFile(path), "before\n");

        std::filesystem::rename(path, moved);
        ASSERT_TRUE(sink->reopen());
        logger.info("after");
        logger.flush();
        EXPECT_EQ(readFile(moved), "before\n");
        EXPECT_EQ(readFile(path), "after\n");
    }
    std::filesystem::remove_all(directory);
}

TEST(OstreamSinkTest, KeepsConcurrentLogLinesWhole)
{
    std::ostringstream output;
    auto sink = std::make_shared<sylar::log::OstreamSink>(output);
    sink->setPattern("%v");
    sylar::log::Logger logger("worker", {sink});

    constexpr int threads = 6;
    constexpr int perThread = 100;
    std::vector<std::thread> workers;
    for (int worker = 0; worker < threads; ++worker)
    {
        workers.emplace_back(
            [&logger, worker]
            {
                for (int item = 0; item < perThread; ++item)
                {
                    SYLAR_LOG_INFO(logger, "{}:{}", worker, item);
                }
            }
        );
    }
    for (auto& worker : workers)
    {
        worker.join();
    }

    std::set<std::string> lines;
    std::istringstream input(output.str());
    for (std::string line; std::getline(input, line);)
    {
        lines.insert(line);
    }
    EXPECT_EQ(lines.size(), threads * perThread);
    EXPECT_TRUE(lines.contains("0:0"));
    EXPECT_TRUE(lines.contains("5:99"));
}

TEST(OstreamSinkTest, ReportsWriteFailures)
{
    std::ostringstream output;
    auto sink = std::make_shared<sylar::log::OstreamSink>(output);
    sylar::log::Logger logger("io", {sink});
    output.setstate(std::ios::badbit);

    EXPECT_THROW(logger.info("write fails"), std::ios_base::failure);
}

TEST(LogLevelTest, ParsesNamesForConfiguration)
{
    EXPECT_EQ(
        sylar::log::parseLevel("warn"),
        sylar::log::LogLevel::WARN
    );
    EXPECT_EQ(
        sylar::log::parseLevel("ERROR"),
        sylar::log::LogLevel::ERROR
    );
    EXPECT_FALSE(sylar::log::parseLevel("warning").has_value());
    EXPECT_EQ(
        sylar::log::toString(sylar::log::LogLevel::FATAL),
        "FATAL"
    );
}

TEST(LoggerTest, KeepsDispatchingAfterOneSinkFails)
{
    std::ostringstream brokenOutput;
    std::ostringstream healthyOutput;
    auto broken = std::make_shared<sylar::log::OstreamSink>(
        brokenOutput
    );
    auto healthy = std::make_shared<sylar::log::OstreamSink>(
        healthyOutput
    );
    brokenOutput.setstate(std::ios::badbit);
    sylar::log::Logger logger("io", {broken, healthy});

    EXPECT_THROW(
        logger.info("still delivered"),
        std::ios_base::failure
    );
    EXPECT_NE(
        healthyOutput.str().find("still delivered"),
        std::string::npos
    );
}
} // namespace
