#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <list>
#include <map>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <gtest/gtest.h>

#include "config/Config.hpp"
#include "config/LogLevelCodec.hpp"
#include "log/Logger.hpp"

namespace
{

struct Person
{
    std::string name;
    int age;

    bool operator==(const Person&) const = default;
};

class TempDirectory
{
public:
    TempDirectory()
        : path(
              std::filesystem::temp_directory_path() /
              ("sylar-config-" +
               std::to_string(std::chrono::steady_clock::now()
                                  .time_since_epoch()
                                  .count()))
          )
    {
        std::filesystem::create_directories(path);
    }

    ~TempDirectory()
    {
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
    }

    std::filesystem::path path;
};

void writeFile(
    const std::filesystem::path& path, const std::string& contents
)
{
    std::ofstream output(path);
    ASSERT_TRUE(output.is_open());
    output << contents;
    ASSERT_TRUE(output.good());
}

} // namespace

namespace sylar
{

template <> struct LexicalCast<std::string, Person>
{
    Person operator()(const std::string& value) const
    {
        const auto node = YAML::Load(value);
        return {
            node["name"].as<std::string>(),
            node["age"].as<int>()};
    }
};

template <> struct LexicalCast<Person, std::string>
{
    std::string operator()(const Person& value) const
    {
        YAML::Node node;
        node["name"] = value.name;
        node["age"] = value.age;
        return YAML::Dump(node);
    }
};

} // namespace sylar

TEST(ConfigTest, RegistersCanonicalNamesAndRejectsTypeMismatch)
{
    auto port = sylar::Config::Lookup<int>(
        "Tests.Registry.Port",
        8080,
        "listen port"
    );
    ASSERT_NE(port, nullptr);
    EXPECT_EQ(port->getName(), "tests.registry.port");
    EXPECT_EQ(port->getDescription(), "listen port");
    EXPECT_EQ(port->getValue(), 8080);
    EXPECT_EQ(
        sylar::Config::Lookup<int>("tests.registry.port", 9999),
        port
    );
    EXPECT_EQ(
        sylar::Config::Lookup<float>("tests.registry.port", 1.0f),
        nullptr
    );
    EXPECT_EQ(sylar::Config::LookupBase("TESTS.REGISTRY.PORT"), port);
    EXPECT_THROW(
        sylar::Config::Lookup<int>("tests..invalid", 1),
        std::invalid_argument
    );

    bool visited = false;
    sylar::Config::Visit(
        [&](const sylar::ConfigVarBase::ptr& variable)
        {
            if (variable == port)
            {
                visited = true;
                EXPECT_EQ(
                    sylar::Config::Lookup<int>("tests.registry.port"),
                    port
                );
            }
        }
    );
    EXPECT_TRUE(visited);
}

TEST(ConfigTest, LoadsNestedYamlContainersAndCustomTypes)
{
    auto port = sylar::Config::Lookup<int>("tests.yaml.port", 0);
    auto names = sylar::Config::Lookup<std::vector<std::string>>(
        "tests.yaml.names",
        {}
    );
    auto tags = sylar::Config::Lookup<std::set<int>>(
        "tests.yaml.tags",
        {}
    );
    auto items = sylar::Config::Lookup<std::list<int>>(
        "tests.yaml.items",
        {}
    );
    auto unique = sylar::Config::Lookup<std::unordered_set<int>>(
        "tests.yaml.unique",
        {}
    );
    auto counts = sylar::Config::Lookup<
        std::unordered_map<std::string, int>>(
        "tests.yaml.counts",
        {}
    );
    auto weights = sylar::Config::Lookup<
        std::map<std::string, std::vector<Person>>>(
        "tests.yaml.weights",
        {}
    );
    auto person = sylar::Config::Lookup<Person>(
        "tests.yaml.person",
        Person{"initial", 0}
    );

    sylar::Config::LoadFromYaml(YAML::Load(R"(
tests:
  yaml:
    port: 8123
    names: [alpha, "a:b"]
    tags: [3, 1, 3]
    items: [8, 9]
    unique: [2, 2, 4]
    counts: {first: 1, second: 2}
    weights:
      "odd-key": [{name: Alice, age: 20}, {name: Bob, age: 30}]
    person: {name: Carol, age: 40}
)"));

    EXPECT_EQ(port->getValue(), 8123);
    EXPECT_EQ(
        names->getValue(),
        (std::vector<std::string>{"alpha", "a:b"})
    );
    EXPECT_EQ(tags->getValue(), (std::set<int>{1, 3}));
    EXPECT_EQ(items->getValue(), (std::list<int>{8, 9}));
    EXPECT_EQ(unique->getValue(), (std::unordered_set<int>{2, 4}));
    EXPECT_EQ(counts->getValue().at("second"), 2);
    EXPECT_EQ(
        weights->getValue().at("odd-key"),
        (std::vector<Person>{{"Alice", 20}, {"Bob", 30}})
    );
    EXPECT_EQ(person->getValue(), (Person{"Carol", 40}));
    const auto encoded = sylar::LexicalCast<
        std::vector<Person>,
        std::string>{}(weights->getValue().at("odd-key"));
    const auto decoded = sylar::
        LexicalCast<std::string, std::vector<Person>>{}(encoded);
    EXPECT_EQ(decoded, weights->getValue().at("odd-key"));
    EXPECT_EQ(
        sylar::Config::Lookup<int>("tests.yaml.unknown"),
        nullptr
    );
}

TEST(ConfigTest, NotifiesAfterUpdateWithoutHoldingValueLock)
{
    auto value = sylar::Config::Lookup<int>(
        "tests.listener.value",
        1
    );
    auto mirror = sylar::Config::Lookup<int>(
        "tests.listener.mirror",
        0
    );
    int calls = 0;
    const auto id = value->addListener(
        [&](int oldValue, int newValue)
        {
            EXPECT_EQ(value->getValue(), newValue);
            EXPECT_EQ(oldValue, 1);
            mirror->setValue(newValue);
            ++calls;
        }
    );

    EXPECT_TRUE(value->fromString("7"));
    EXPECT_EQ(mirror->getValue(), 7);
    EXPECT_EQ(calls, 1);
    EXPECT_TRUE(value->fromString("7"));
    EXPECT_FALSE(value->fromString("not-an-int"));
    EXPECT_EQ(calls, 1);
    EXPECT_EQ(value->getValue(), 7);
    EXPECT_TRUE(static_cast<bool>(value->getListener(id)));
    value->delListener(id);
    EXPECT_FALSE(static_cast<bool>(value->getListener(id)));
    value->setValue(8);
    EXPECT_EQ(calls, 1);
}

TEST(ConfigTest, ReloadsChangedDirectoryAndRetriesInvalidYaml)
{
    TempDirectory directory;
    std::filesystem::create_directories(directory.path / "nested");
    const auto first = directory.path / "a.yml";
    const auto last = directory.path / "nested" / "z.yaml";
    writeFile(first, "tests:\n  directory:\n    value: 10\n");
    writeFile(
        last,
        "tests:\n  directory:\n    value: 20\n    later: 77\n"
    );
    auto value = sylar::Config::Lookup<int>(
        "tests.directory.value",
        0
    );

    int changes = 0;
    const auto listenerId = value->addListener([&](int, int)
                                               { ++changes; });
    sylar::Config::LoadFromConfDir(directory.path);
    EXPECT_EQ(value->getValue(), 20);
    EXPECT_EQ(changes, 1);
    auto later = sylar::Config::Lookup<int>(
        "tests.directory.later",
        0
    );
    sylar::Config::LoadFromConfDir(directory.path);
    EXPECT_EQ(later->getValue(), 77);
    writeFile(first, "tests:\n  directory:\n    value: 1000\n");
    sylar::Config::LoadFromConfDir(directory.path);
    EXPECT_EQ(value->getValue(), 20);
    EXPECT_EQ(changes, 1);

    writeFile(last, "tests: [invalid\n");
    EXPECT_THROW(
        sylar::Config::LoadFromConfDir(directory.path),
        YAML::Exception
    );
    EXPECT_EQ(value->getValue(), 20);

    writeFile(last, "tests:\n  directory:\n    value: 45\n");
    sylar::Config::LoadFromConfDir(directory.path);
    EXPECT_EQ(value->getValue(), 45);
    sylar::Config::LoadFromConfDir(directory.path, true);
    EXPECT_EQ(value->getValue(), 45);
    EXPECT_EQ(changes, 2);
    value->delListener(listenerId);
}

TEST(ConfigTest, AppliesYamlLevelToCurrentLogger)
{
    sylar::log::Logger logger("config-test", {});
    auto level = sylar::Config::Lookup<sylar::log::LogLevel>(
        "tests.logging.level",
        sylar::log::LogLevel::INFO
    );
    const auto id = level->addListener([&](sylar::log::LogLevel,
                                           sylar::log::LogLevel next)
                                       { logger.setLevel(next); });

    sylar::Config::LoadFromYaml(
        YAML::Load("tests:\n  logging:\n    level: error\n")
    );
    EXPECT_FALSE(logger.shouldLog(sylar::log::LogLevel::INFO));
    EXPECT_TRUE(logger.shouldLog(sylar::log::LogLevel::ERROR));
    EXPECT_EQ(level->getValue(), sylar::log::LogLevel::ERROR);
    EXPECT_THROW(
        sylar::Config::LoadFromYaml(
            YAML::Load("tests:\n  logging:\n    level: bogus\n")
        ),
        std::invalid_argument
    );
    EXPECT_EQ(level->getValue(), sylar::log::LogLevel::ERROR);
    level->delListener(id);
}
TEST(ConfigTest, ConcurrentUpdatesKeepValueAndListenersSafe)
{
    auto value = sylar::Config::Lookup<int>(
        "tests.concurrent.value",
        -1
    );
    std::atomic<int> calls = 0;
    const auto id = value->addListener([&](int, int)
                                       { calls.fetch_add(1); });

    std::vector<std::thread> workers;
    for (int worker = 0; worker < 4; ++worker)
    {
        workers.emplace_back(
            [=]
            {
                for (int index = 0; index < 100; ++index)
                {
                    value->setValue(worker * 100 + index);
                }
            }
        );
    }
    for (auto& worker : workers)
    {
        worker.join();
    }
    EXPECT_EQ(calls.load(), 400);
    value->delListener(id);
}