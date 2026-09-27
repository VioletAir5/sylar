#include "log/FileSink.hpp"

#include <ios>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace sylar::log
{

FileSink::FileSink(std::filesystem::path path)
    : m_path(std::move(path))
{
    if (!reopen())
    {
        throw std::runtime_error(
            "failed to open log file: " + m_path.string()
        );
    }
}

bool FileSink::reopen()
{
    std::lock_guard lock(m_mutex);
    const auto parent = m_path.parent_path();
    if (!parent.empty())
    {
        std::error_code error;
        std::filesystem::create_directories(parent, error);
        if (error)
        {
            return false;
        }
    }

    std::ofstream next(m_path, std::ios::app);
    if (!next.is_open())
    {
        return false;
    }
    m_file = std::move(next);
    return true;
}

void FileSink::writeFormatted(std::string_view line)
{
    m_file.write(
        line.data(),
        static_cast<std::streamsize>(line.size())
    );
    if (!m_file)
    {
        throw std::ios_base::failure(
            "failed to write log file: " + m_path.string()
        );
    }
}

void FileSink::flushOutput()
{
    m_file.flush();
    if (!m_file)
    {
        throw std::ios_base::failure(
            "failed to flush log file: " + m_path.string()
        );
    }
}

} // namespace sylar::log
