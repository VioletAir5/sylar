#include "log/PatternFormatter.hpp"

#include <ctime>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace sylar::log
{

PatternFormatter::PatternFormatter(std::string pattern)
    : m_pattern(std::move(pattern))
{
    std::string literal;
    const auto emitLiteral = [&]
    {
        if (!literal.empty())
        {
            m_tokens.push_back({Field::LITERAL, std::move(literal)});
            literal.clear();
        }
    };

    for (std::size_t index = 0; index < m_pattern.size(); ++index)
    {
        if (m_pattern[index] != '%')
        {
            literal.push_back(m_pattern[index]);
            continue;
        }
        if (++index == m_pattern.size())
        {
            throw std::invalid_argument("log pattern ends with %");
        }
        const char flag = m_pattern[index];
        if (flag == '%')
        {
            literal.push_back('%');
            continue;
        }

        Field field;
        switch (flag)
        {
        case 'Y':
            field = Field::YEAR;
            break;
        case 'm':
            field = Field::MONTH;
            break;
        case 'd':
            field = Field::DAY;
            break;
        case 'H':
            field = Field::HOUR;
            break;
        case 'M':
            field = Field::MINUTE;
            break;
        case 'S':
            field = Field::SECOND;
            break;
        case 'n':
            field = Field::LOGGER;
            break;
        case 'l':
            field = Field::LEVEL;
            break;
        case 'v':
            field = Field::MESSAGE;
            break;
        case 't':
            field = Field::THREAD;
            break;
        case 's':
            field = Field::SOURCE_FILE;
            break;
        case '#':
            field = Field::SOURCE_LINE;
            break;
        default:
            throw std::invalid_argument(
                "unknown log pattern flag: %" + std::string(1, flag)
            );
        }
        emitLiteral();
        m_tokens.push_back({field, {}});
    }
    emitLiteral();
}

std::string PatternFormatter::format(const LogRecord& record) const
{
    const auto seconds = std::chrono::system_clock::to_time_t(
        record.m_timeStamp
    );
    std::tm localTime{};
    if (localtime_r(&seconds, &localTime) == nullptr)
    {
        throw std::runtime_error("failed to format log timestamp");
    }

    std::ostringstream out;
    for (const auto& token : m_tokens)
    {
        switch (token.field)
        {
        case Field::LITERAL:
            out << token.text;
            break;
        case Field::YEAR:
            out << std::put_time(&localTime, "%Y");
            break;
        case Field::MONTH:
            out << std::put_time(&localTime, "%m");
            break;
        case Field::DAY:
            out << std::put_time(&localTime, "%d");
            break;
        case Field::HOUR:
            out << std::put_time(&localTime, "%H");
            break;
        case Field::MINUTE:
            out << std::put_time(&localTime, "%M");
            break;
        case Field::SECOND:
            out << std::put_time(&localTime, "%S");
            break;
        case Field::LOGGER:
            out << record.m_name;
            break;
        case Field::LEVEL:
            out << toString(record.m_level);
            break;
        case Field::MESSAGE:
            out << record.m_message;
            break;
        case Field::THREAD:
            out << record.m_threadId;
            break;
        case Field::SOURCE_FILE:
        {
            const std::string_view file = record.m_source.file_name();
            const auto slash = file.find_last_of("/\\");
            out << file.substr(
                slash == std::string_view::npos ? 0 : slash + 1
            );
            break;
        }
        case Field::SOURCE_LINE:
            out << record.m_source.line();
            break;
        }
    }
    return out.str();
}

} // namespace sylar::log
