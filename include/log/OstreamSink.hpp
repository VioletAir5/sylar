#pragma once

#include <ostream>

#include "log/Sink.hpp"

namespace sylar::log
{

class OstreamSink final : public Sink
{
public:
    explicit OstreamSink(std::ostream& output)
        : m_output(output)
    {
    }

protected:
    void writeFormatted(std::string_view line) override;
    void flushOutput() override;

private:
    std::ostream& m_output;
};

} // namespace sylar::log
