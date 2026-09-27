#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "log/Logger.hpp"

namespace
{

class RecordingSink final : public sylar::log::Sink
{
public:
    std::vector<std::string> lines;
    int flushCount = 0;

protected:
    void writeFormatted(std::string_view line) override
    {
        lines.emplace_back(line);
    }

    void flushOutput() override
    {
        ++flushCount;
    }
};

TEST(LoggerTest, FansOutOneRecordToEverySink)
{
    auto first = std::make_shared<RecordingSink>();
    auto second = std::make_shared<RecordingSink>();
    first->setPattern("%n|%l|%v");
    second->setPattern("%v");
    sylar::log::Logger logger("network", {first, second});

    logger.info("connected");

    ASSERT_EQ(first->lines.size(), 1);
    ASSERT_EQ(second->lines.size(), 1);
    EXPECT_EQ(first->lines[0], "network|INFO|connected\n");
    EXPECT_EQ(second->lines[0], "connected\n");
}

TEST(LoggerTest, FiltersAtLoggerAndSink)
{
    auto first = std::make_shared<RecordingSink>();
    auto second = std::make_shared<RecordingSink>();
    first->setPattern("%v");
    second->setPattern("%v");
    second->setLevel(sylar::log::LogLevel::ERROR);
    sylar::log::Logger logger(
        "network",
        {first, second},
        sylar::log::LogLevel::WARN
    );

    logger.debug("hidden");
    logger.warn("first only");
    logger.error("both");

    ASSERT_EQ(first->lines.size(), 2);
    ASSERT_EQ(second->lines.size(), 1);
    EXPECT_EQ(first->lines[0], "first only\n");
    EXPECT_EQ(first->lines[1], "both\n");
    EXPECT_EQ(second->lines[0], "both\n");

    logger.setLevel(sylar::log::LogLevel::OFF);
    logger.fatal("hidden too");
    EXPECT_EQ(first->lines.size(), 2);
}

TEST(LoggerTest, FlushesEverySinkAndSupportsFlushOn)
{
    auto first = std::make_shared<RecordingSink>();
    auto second = std::make_shared<RecordingSink>();
    sylar::log::Logger logger("network", {first, second});
    logger.flushOn(sylar::log::LogLevel::ERROR);

    logger.info("buffered");
    EXPECT_EQ(first->flushCount, 0);
    logger.error("flushed");
    EXPECT_EQ(first->flushCount, 1);
    EXPECT_EQ(second->flushCount, 1);
    logger.flush();
    EXPECT_EQ(first->flushCount, 2);
    EXPECT_EQ(second->flushCount, 2);
}

TEST(LoggerTest, RejectsNullSink)
{
    EXPECT_THROW(
        (sylar::log::Logger{"network", {nullptr}}),
        std::invalid_argument
    );
}

TEST(
    LoggerTest, FormattedMacroCapturesCallerAndSkipsDisabledArguments
)
{
    auto sink = std::make_shared<RecordingSink>();
    sink->setPattern("%s:%#|%v");
    sylar::log::Logger logger(
        "network",
        {sink},
        sylar::log::LogLevel::INFO
    );

    int evaluations = 0;
    SYLAR_LOG_DEBUG(logger, "hidden {}", ++evaluations);
    EXPECT_EQ(evaluations, 0);

    SYLAR_LOG_INFO(logger, "connection {}", 42);
    ASSERT_EQ(sink->lines.size(), 1);
    EXPECT_NE(
        sink->lines[0].find("test_logger.cpp:"),
        std::string::npos
    );
    EXPECT_NE(
        sink->lines[0].find("connection 42"),
        std::string::npos
    );
}

} // namespace
