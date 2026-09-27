#include "config/Config.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <map>
#include <stdexcept>
#include <utility>
#include <vector>

namespace sylar
{
namespace
{

std::string canonicalName(std::string_view name)
{
    if (name.empty() || name.front() == '.' || name.back() == '.')
    {
        throw std::invalid_argument("invalid config name");
    }

    std::string result;
    result.reserve(name.size());
    bool lastWasDot = false;
    for (unsigned char character : name)
    {
        if (character == '.')
        {
            if (lastWasDot)
            {
                throw std::invalid_argument("invalid config name");
            }
            lastWasDot = true;
            result.push_back('.');
        }
        else
        {
            lastWasDot = false;
            const auto lower = static_cast<char>(
                std::tolower(character)
            );
            if ((lower < 'a' || lower > 'z') &&
                (lower < '0' || lower > '9') && lower != '_')
            {
                throw std::invalid_argument("invalid config name");
            }
            result.push_back(lower);
        }
    }
    return result;
}

struct FileStamp
{
    std::filesystem::file_time_type modified;
    std::uintmax_t size;

    bool operator==(const FileStamp&) const = default;
};

using FileState = std::map<std::filesystem::path, FileStamp>;

struct DirectoryState
{
    FileState files;
    std::uint64_t generation;
};

std::mutex& reloadMutex()
{
    static std::mutex value;
    return value;
}

std::map<std::filesystem::path, DirectoryState>& loadedDirectories()
{
    static std::map<std::filesystem::path, DirectoryState> value;
    return value;
}

using PendingValues = std::
    map<std::string, std::pair<ConfigVarBase::ptr, std::string>>;

void collectNode(
    std::string_view prefix,
    const YAML::Node& node,
    PendingValues& pending
)
{
    if (!prefix.empty())
    {
        ConfigVarBase::ptr variable;
        try
        {
            variable = Config::LookupBase(prefix);
        }
        catch (const std::invalid_argument&)
        {
            // Map values may have keys that are not config paths.
            return;
        }
        if (variable)
        {
            const std::string text = node.IsScalar()
                                         ? node.Scalar()
                                         : YAML::Dump(node);
            pending[variable->getName()] = {variable, text};
        }
    }

    if (!node.IsMap())
    {
        return;
    }

    for (const auto& entry : node)
    {
        const auto segment = entry.first.as<std::string>();
        const auto child = prefix.empty()
                               ? segment
                               : std::string(prefix) + "." + segment;
        collectNode(child, entry.second, pending);
    }
}

void applyPending(const PendingValues& pending)
{
    for (const auto& [name, value] : pending)
    {
        const auto& [variable, text] = value;
        if (!variable->fromString(text))
        {
            throw std::invalid_argument(
                "invalid YAML value for config " + name
            );
        }
    }
}
} // namespace

ConfigVarBase::ConfigVarBase(
    std::string name, std::string description
)
    : m_name(canonicalName(name))
    , m_description(std::move(description))
{
}

std::string Config::normalizeName(std::string_view name)
{
    return canonicalName(name);
}

Config::ConfigVarMap& Config::data()
{
    static ConfigVarMap values;
    return values;
}

std::shared_mutex& Config::mutex()
{
    static std::shared_mutex value;
    return value;
}

std::atomic<std::uint64_t>& Config::generation()
{
    static std::atomic<std::uint64_t> value{0};
    return value;
}

ConfigVarBase::ptr Config::LookupBase(std::string_view name)
{
    const std::string key = normalizeName(name);
    std::shared_lock lock(mutex());
    const auto it = data().find(key);
    return it == data().end() ? nullptr : it->second;
}

void Config::LoadFromYaml(const YAML::Node& root)
{
    if (!root.IsMap())
    {
        throw std::invalid_argument(
            "configuration root must be a YAML map"
        );
    }
    PendingValues pending;
    collectNode("", root, pending);
    applyPending(pending);
}

void Config::LoadFromConfDir(
    const std::filesystem::path& path, bool force
)
{
    const auto directory = std::filesystem::absolute(path)
                               .lexically_normal();
    if (!std::filesystem::is_directory(directory))
    {
        throw std::invalid_argument(
            "config directory does not exist: " + directory.string()
        );
    }

    FileState current;
    for (const auto& entry :
         std::filesystem::recursive_directory_iterator(directory))
    {
        if (!entry.is_regular_file())
        {
            continue;
        }
        const auto extension = entry.path().extension();
        if (extension != ".yml" && extension != ".yaml")
        {
            continue;
        }
        const auto file = entry.path().lexically_normal();
        current.emplace(
            file,
            FileStamp{entry.last_write_time(), entry.file_size()}
        );
    }

    const auto registryGeneration = generation().load(
        std::memory_order_relaxed
    );
    {
        std::lock_guard reloadLock(reloadMutex());
        const auto previous = loadedDirectories().find(directory);
        if (!force && previous != loadedDirectories().end() &&
            previous->second.files == current &&
            previous->second.generation == registryGeneration)
        {
            return;
        }
    }

    // Parse every file before applying any of them. Reload all files
    // to preserve deterministic overrides when an earlier file
    // changes.
    std::vector<YAML::Node> parsed;
    parsed.reserve(current.size());
    for (const auto& [file, stamp] : current)
    {
        (void)stamp;
        parsed.push_back(YAML::LoadFile(file.string()));
    }
    PendingValues pending;
    for (const auto& root : parsed)
    {
        if (!root.IsMap())
        {
            throw std::invalid_argument(
                "configuration root must be a YAML map"
            );
        }
        collectNode("", root, pending);
    }
    applyPending(pending);
    {
        std::lock_guard reloadLock(reloadMutex());
        loadedDirectories()[directory] = DirectoryState{
            std::move(current),
            registryGeneration};
    }
}

void Config::Visit(
    const std::function<void(ConfigVarBase::ptr)>& callback
)
{
    if (!callback)
    {
        throw std::invalid_argument("config visitor must not be empty"
        );
    }

    std::vector<ConfigVarBase::ptr> snapshot;
    {
        std::shared_lock lock(mutex());
        snapshot.reserve(data().size());
        for (const auto& [name, variable] : data())
        {
            (void)name;
            snapshot.push_back(variable);
        }
    }
    for (const auto& variable : snapshot)
    {
        callback(variable);
    }
}

} // namespace sylar