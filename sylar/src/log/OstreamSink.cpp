#include "log/OstreamSink.hpp"

#include <ios>

namespace sylar::log
{

void OstreamSink::writeFormatted(std::string_view line)
{
    m_output.write(
        line.data(),
        static_cast<std::streamsize>(line.size())
    );
    if (!m_output)
    {
        throw std::ios_base::failure(
            "failed to write log to output stream"
        );
    }
}

void OstreamSink::flushOutput()
{
    m_output.flush();
    if (!m_output)
    {
        throw std::ios_base::failure("failed to flush output stream");
    }
}

} // namespace sylar::log
