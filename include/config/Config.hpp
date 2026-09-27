#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <typeinfo>
#include <unordered_map>
#include <utility>
#include <vector>

#include <yaml-cpp/yaml.h>

#include "config/YamlCodec.hpp"

namespace sylar
{

class ConfigVarBase
{
public:
    using ptr = std::shared_ptr<ConfigVarBase>;

    ConfigVarBase(std::string name, std::string description = "");
    virtual ~ConfigVarBase() = default;

    const std::string& getName() const noexcept
    {
        return m_name;
    }
    const std::string& getDescription() const noexcept
    {
        return m_description;
    }

    virtual std::string toString() const = 0;
    virtual bool fromString(const std::string& value) = 0;
    virtual std::string getTypeName() const = 0;

private:
    std::string m_name;
    std::string m_description;
};

template <
    class T,
    class FromStr = LexicalCast<std::string, T>,
    class ToStr = LexicalCast<T, std::string>>
class ConfigVar final : public ConfigVarBase
{
public:
    using ptr = std::shared_ptr<ConfigVar>;
    using on_change_cb = std::function<void(const T&, const T&)>;

    ConfigVar(
        std::string name, T defaultValue, std::string description = ""
    )
        : ConfigVarBase(std::move(name), std::move(description))
        , m_value(std::move(defaultValue))
    {
    }

    std::string toString() const override
    {
        std::shared_lock lock(m_mutex);
        return ToStr{}(m_value);
    }

    bool fromString(const std::string& text) override
    {
        std::optional<T> parsed;
        try
        {
            parsed.emplace(FromStr{}(text));
        }
        catch (const std::exception&)
        {
            return false;
        }
        setValue(std::move(*parsed));
        return true;
    }

    std::string getTypeName() const override
    {
        return typeid(T).name();
    }

    T getValue() const
    {
        std::shared_lock lock(m_mutex);
        return m_value;
    }

    void setValue(T value)
    {
        std::optional<T> oldValue;
        std::optional<T> newValue;
        std::vector<on_change_cb> listeners;
        {
            std::unique_lock lock(m_mutex);
            if (m_value == value)
            {
                return;
            }
            oldValue.emplace(m_value);
            m_value = std::move(value);
            newValue.emplace(m_value);
            listeners.reserve(m_listeners.size());
            for (const auto& [id, listener] : m_listeners)
            {
                (void)id;
                listeners.push_back(listener);
            }
        }

        // A listener can read or update this variable without
        // deadlocking.
        for (const auto& listener : listeners)
        {
            listener(*oldValue, *newValue);
        }
    }

    std::uint64_t addListener(on_change_cb listener)
    {
        if (!listener)
        {
            throw std::invalid_argument(
                "config listener must not be empty"
            );
        }

        std::unique_lock lock(m_mutex);
        if (m_nextListenerId ==
            std::numeric_limits<std::uint64_t>::max())
        {
            throw std::overflow_error("config listener id exhausted");
        }
        const auto id = ++m_nextListenerId;
        m_listeners.emplace(id, std::move(listener));
        return id;
    }

    void delListener(std::uint64_t id)
    {
        std::unique_lock lock(m_mutex);
        m_listeners.erase(id);
    }

    on_change_cb getListener(std::uint64_t id) const
    {
        std::shared_lock lock(m_mutex);
        const auto it = m_listeners.find(id);
        return it == m_listeners.end() ? on_change_cb{} : it->second;
    }

    void clearListener()
    {
        std::unique_lock lock(m_mutex);
        m_listeners.clear();
    }

private:
    mutable std::shared_mutex m_mutex;
    T m_value;
    std::map<std::uint64_t, on_change_cb> m_listeners;
    std::uint64_t m_nextListenerId = 0;
};

class Config
{
public:
    using ConfigVarMap = std::
        unordered_map<std::string, ConfigVarBase::ptr>;

    template <class T>
    static typename ConfigVar<T>::ptr Lookup(
        std::string_view name,
        T defaultValue,
        std::string description = ""
    )
    {
        const std::string key = normalizeName(name);
        std::unique_lock lock(mutex());
        const auto it = data().find(key);
        if (it != data().end())
        {
            return std::dynamic_pointer_cast<ConfigVar<T>>(it->second
            );
        }

        auto variable = std::make_shared<ConfigVar<T>>(
            key,
            std::move(defaultValue),
            std::move(description)
        );
        data().emplace(key, variable);
        generation().fetch_add(1, std::memory_order_relaxed);
        return variable;
    }

    template <class T>
    static typename ConfigVar<T>::ptr Lookup(std::string_view name)
    {
        const std::string key = normalizeName(name);
        std::shared_lock lock(mutex());
        const auto it = data().find(key);
        return it == data().end()
                   ? nullptr
                   : std::dynamic_pointer_cast<ConfigVar<T>>(
                         it->second
                     );
    }

    static ConfigVarBase::ptr LookupBase(std::string_view name);
    static void LoadFromYaml(const YAML::Node& root);
    static void LoadFromConfDir(
        const std::filesystem::path& path, bool force = false
    );
    static void
    Visit(const std::function<void(ConfigVarBase::ptr)>& callback);

private:
    static std::string normalizeName(std::string_view name);
    static ConfigVarMap& data();
    static std::shared_mutex& mutex();
    static std::atomic<std::uint64_t>& generation();
};

} // namespace sylar