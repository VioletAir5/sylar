#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <unordered_set>
#include <utility>
#include <vector>

#include "db/Sqlite.hpp"

namespace sylar
{

template <class Entity, class Field> struct SqliteColumn
{
    std::string name;
    Field Entity::*member;
};

template <class Entity, class Field>
SqliteColumn<Entity, Field>
sqliteColumn(std::string name, Field Entity::*member)
{
    return {std::move(name), member};
}

namespace detail
{

inline void validateIdentifier(std::string_view name)
{
    if (name.empty())
    {
        throw std::invalid_argument("empty SQL identifier");
    }
    for (std::size_t i = 0; i < name.size(); ++i)
    {
        const auto ch = static_cast<unsigned char>(name[i]);
        const bool letter = (ch >= 'a' && ch <= 'z') ||
                            (ch >= 'A' && ch <= 'Z') || ch == '_';
        const bool digit = ch >= '0' && ch <= '9';
        if (!letter && (i == 0 || !digit))
        {
            throw std::invalid_argument("invalid SQL identifier");
        }
    }
}

inline std::string quoteIdentifier(std::string_view name)
{
    return "\"" + std::string(name) + "\"";
}

inline std::string identifierKey(std::string_view name)
{
    std::string result(name);
    for (char& ch : result)
    {
        if (ch >= 'A' && ch <= 'Z')
        {
            ch = static_cast<char>(ch - 'A' + 'a');
        }
    }
    return result;
}

template <class T, class = void> struct SqliteCodec;

template <class T>
struct SqliteCodec<T, std::enable_if_t<std::is_integral_v<T>>>
{
    static constexpr std::string_view sqlType = "INTEGER";

    static void bind(SqliteStatement& statement, int index, T value)
    {
        if (!std::in_range<std::int64_t>(value))
        {
            throw std::out_of_range(
                "ORM integer does not fit SQLite INTEGER"
            );
        }
        statement.bind(index, static_cast<std::int64_t>(value));
    }

    static T read(const SqliteStatement& statement, int index)
    {
        const auto value = statement.columnInt64(index);
        if (!std::in_range<T>(value))
        {
            throw std::out_of_range(
                "SQLite INTEGER does not fit ORM field"
            );
        }
        return static_cast<T>(value);
    }
};

template <class T>
struct SqliteCodec<T, std::enable_if_t<std::is_floating_point_v<T>>>
{
    static constexpr std::string_view sqlType = "REAL";

    static void bind(SqliteStatement& statement, int index, T value)
    {
        statement.bind(index, static_cast<double>(value));
    }

    static T read(const SqliteStatement& statement, int index)
    {
        return static_cast<T>(statement.columnDouble(index));
    }
};

template <> struct SqliteCodec<std::string>
{
    static constexpr std::string_view sqlType = "TEXT";

    static void bind(
        SqliteStatement& statement,
        int index,
        const std::string& value
    )
    {
        statement.bind(index, value);
    }

    static std::string
    read(const SqliteStatement& statement, int index)
    {
        return statement.columnText(index);
    }
};

template <> struct SqliteCodec<std::vector<std::byte>>
{
    static constexpr std::string_view sqlType = "BLOB";

    static void bind(
        SqliteStatement& statement,
        int index,
        const std::vector<std::byte>& value
    )
    {
        statement.bindBlob(index, value);
    }

    static std::vector<std::byte>
    read(const SqliteStatement& statement, int index)
    {
        return statement.columnBlob(index);
    }
};

template <class T> struct SqliteCodec<std::optional<T>>
{
    static constexpr std::string_view
        sqlType = SqliteCodec<T>::sqlType;

    static void bind(
        SqliteStatement& statement,
        int index,
        const std::optional<T>& value
    )
    {
        if (value)
        {
            SqliteCodec<T>::bind(statement, index, *value);
        }
        else
        {
            statement.bindNull(index);
        }
    }

    static std::optional<T>
    read(const SqliteStatement& statement, int index)
    {
        if (statement.columnIsNull(index))
        {
            return std::nullopt;
        }
        return SqliteCodec<T>::read(statement, index);
    }
};

} // namespace detail

template <class Entity, class... Fields> class SqliteRepository
{
public:
    SqliteRepository(
        SqliteDatabase& database,
        std::string table,
        std::int64_t Entity::*id,
        SqliteColumn<Entity, Fields>... columns
    )
        : m_database(database)
        , m_table(std::move(table))
        , m_id(id)
        , m_columns(std::move(columns)...)
    {
        static_assert(std::is_default_constructible_v<Entity>);
        if (!m_id)
        {
            throw std::invalid_argument("null ORM id member");
        }
        detail::validateIdentifier(m_table);
        std::unordered_set<std::string> names{"id"};
        auto validateColumn = [&names](const auto& column)
        {
            detail::validateIdentifier(column.name);
            if (!column.member ||
                !names.insert(detail::identifierKey(column.name))
                     .second)
            {
                throw std::invalid_argument(
                    "invalid ORM column mapping"
                );
            }
        };
        std::apply(
            [&validateColumn](const auto&... column)
            { (validateColumn(column), ...); },
            m_columns
        );
        buildSql();
    }

    void createTable()
    {
        m_database.execute(m_create);
    }

    void save(const Entity& entity)
    {
        auto statement = m_database.prepare(m_upsert);
        statement.bind(1, entity.*m_id);
        bindFields(
            statement,
            entity,
            std::index_sequence_for<Fields...>{}
        );
        statement.step();
    }

    std::optional<Entity> find(std::int64_t id)
    {
        auto statement = m_database.prepare(m_select);
        statement.bind(1, id);
        if (!statement.step())
        {
            return std::nullopt;
        }
        Entity entity{};
        entity.*m_id = id;
        readFields(
            statement,
            entity,
            std::index_sequence_for<Fields...>{}
        );
        return entity;
    }

    void erase(std::int64_t id)
    {
        auto statement = m_database.prepare(m_delete);
        statement.bind(1, id);
        statement.step();
    }

private:
    void buildSql()
    {
        m_create = "CREATE TABLE IF NOT EXISTS " +
                   detail::quoteIdentifier(m_table) +
                   "(id INTEGER PRIMARY KEY";
        m_upsert = "INSERT INTO " + detail::quoteIdentifier(m_table) +
                   "(id";
        m_select = "SELECT ";
        m_delete = "DELETE FROM " + detail::quoteIdentifier(m_table) +
                   " WHERE id = ?1";
        std::apply(
            [this](const auto&... column)
            {
                ((m_create +=
                  ", " + detail::quoteIdentifier(column.name) + " " +
                  std::string(
                      detail::SqliteCodec<std::remove_cvref_t<
                          decltype(std::declval<Entity>().*column.member)>>::
                          sqlType
                  ),
                  m_upsert += ", " +
                              detail::quoteIdentifier(column.name)),
                 ...);
            },
            m_columns
        );
        m_create += ")";
        m_upsert += ") VALUES(?1";
        for (std::size_t i = 0; i < sizeof...(Fields); ++i)
        {
            m_upsert += ", ?" + std::to_string(i + 2);
        }
        m_upsert += ")";
        if constexpr (sizeof...(Fields) == 0)
        {
            m_upsert += " ON CONFLICT(id) "
                        "DO NOTHING";
            m_select = "SELECT id";
        }
        else
        {
            m_upsert += " ON CONFLICT(id) "
                        "DO UPDATE SET ";
            bool first = true;
            std::apply(
                [this, &first](const auto&... column)
                {
                    ((m_upsert += (std::exchange(first, false)
                                       ? ""
                                       : ", ") +
                                  detail::quoteIdentifier(column.name
                                  ) +
                                  " = excluded." +
                                  detail::quoteIdentifier(column.name)
                     ),
                     ...);
                },
                m_columns
            );
            m_select = "SELECT ";
            bool first_select = true;
            std::apply(
                [this, &first_select](const auto&... column)
                {
                    ((m_select += (std::exchange(first_select, false)
                                       ? ""
                                       : ", ") +
                                  detail::quoteIdentifier(column.name)
                     ),
                     ...);
                },
                m_columns
            );
        }
        m_select += " FROM " + detail::quoteIdentifier(m_table) +
                    " WHERE id = ?1";
    }

    template <std::size_t... Index>
    void
    bindFields(SqliteStatement& statement, const Entity& entity, std::index_sequence<Index...>)
    {
        (detail::SqliteCodec<Fields>::bind(
             statement,
             static_cast<int>(Index + 2),
             entity.*std::get<Index>(m_columns).member
         ),
         ...);
    }

    template <std::size_t... Index>
    void
    readFields(const SqliteStatement& statement, Entity& entity, std::index_sequence<Index...>)
    {
        ((entity.*std::get<Index>(m_columns)
                      .member = detail::SqliteCodec<Fields>::read(
              statement,
              static_cast<int>(Index)
          )),
         ...);
    }

    SqliteDatabase& m_database;
    std::string m_table;
    std::int64_t Entity::*m_id;
    std::tuple<SqliteColumn<Entity, Fields>...> m_columns;
    std::string m_create;
    std::string m_upsert;
    std::string m_select;
    std::string m_delete;
};

} // namespace sylar
