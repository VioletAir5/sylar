#pragma once

#include <string>
#include <vector>

#include "log/Formatter.hpp"

namespace sylar::log
{

class PatternFormatter final : public Formatter
{
public:
    explicit PatternFormatter(
        std::string pattern = "%Y-%m-%d %H:%M:%S [%n] [%l] %v"
    );

    const std::string& pattern() const noexcept
    {
        return m_pattern;
    }

    std::string format(const LogRecord& record) const override;

private:
    enum class Field
    {
        LITERAL,
        YEAR,
        MONTH,
        DAY,
        HOUR,
        MINUTE,
        SECOND,
        LOGGER,
        LEVEL,
        MESSAGE,
        THREAD,
        SOURCE_FILE,
        SOURCE_LINE
    };

    struct Token
    {
        Field field;
        std::string text;
    };

    std::string m_pattern;
    std::vector<Token> m_tokens;
};

} // namespace sylar::log
