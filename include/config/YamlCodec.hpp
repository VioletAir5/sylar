#pragma once

#include <list>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <yaml-cpp/yaml.h>

namespace sylar
{

template <class From, class To> struct LexicalCast;

namespace detail
{

template <class T> T decodeElement(const YAML::Node& node)
{
    if constexpr (std::is_same_v<T, std::string>)
    {
        return node.as<std::string>();
    }
    else
    {
        return LexicalCast<std::string, T>{}(YAML::Dump(node));
    }
}

template <class T> YAML::Node encodeElement(const T& value)
{
    if constexpr (std::is_same_v<T, std::string>)
    {
        return YAML::Node(value);
    }
    else
    {
        return YAML::Load(LexicalCast<T, std::string>{}(value));
    }
}

template <class Container> struct SequenceCodec
{
    static Container decode(const YAML::Node& node)
    {
        if (!node.IsSequence())
        {
            throw std::invalid_argument("expected YAML sequence");
        }

        Container values;
        for (const auto& element : node)
        {
            values.insert(
                values.end(),
                decodeElement<typename Container::value_type>(element)
            );
        }
        return values;
    }

    static YAML::Node encode(const Container& values)
    {
        YAML::Node node(YAML::NodeType::Sequence);
        for (const auto& value : values)
        {
            node.push_back(encodeElement(value));
        }
        return node;
    }
};

template <class Map> struct MapCodec
{
    static Map decode(const YAML::Node& node)
    {
        if (!node.IsMap())
        {
            throw std::invalid_argument("expected YAML map");
        }

        Map values;
        for (const auto& entry : node)
        {
            values.emplace(
                entry.first.as<std::string>(),
                decodeElement<typename Map::mapped_type>(entry.second)
            );
        }
        return values;
    }

    static YAML::Node encode(const Map& values)
    {
        YAML::Node node(YAML::NodeType::Map);
        for (const auto& [key, value] : values)
        {
            node[key] = encodeElement(value);
        }
        return node;
    }
};

template <class T> struct YamlCodec
{
    static T decode(const YAML::Node& node)
    {
        return node.as<T>();
    }

    static YAML::Node encode(const T& value)
    {
        return YAML::Node(value);
    }
};

template <class T>
struct YamlCodec<std::vector<T>> : SequenceCodec<std::vector<T>>
{
};
template <class T>
struct YamlCodec<std::list<T>> : SequenceCodec<std::list<T>>
{
};
template <class T>
struct YamlCodec<std::set<T>> : SequenceCodec<std::set<T>>
{
};
template <class T>
struct YamlCodec<std::unordered_set<T>>
    : SequenceCodec<std::unordered_set<T>>
{
};
template <class T>
struct YamlCodec<std::map<std::string, T>>
    : MapCodec<std::map<std::string, T>>
{
};
template <class T>
struct YamlCodec<std::unordered_map<std::string, T>>
    : MapCodec<std::unordered_map<std::string, T>>
{
};

} // namespace detail

template <class T> struct LexicalCast<std::string, T>
{
    T operator()(const std::string& value) const
    {
        return detail::YamlCodec<T>::decode(YAML::Load(value));
    }
};

template <class T> struct LexicalCast<T, std::string>
{
    std::string operator()(const T& value) const
    {
        return YAML::Dump(detail::YamlCodec<T>::encode(value));
    }
};

template <> struct LexicalCast<std::string, std::string>
{
    std::string operator()(const std::string& value) const
    {
        return value;
    }
};

} // namespace sylar