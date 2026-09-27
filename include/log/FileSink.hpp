#pragma once

#include <filesystem>
#include <fstream>

#include "log/Sink.hpp"

namespace sylar::log
{

class FileSink final : public Sink
{
public:
    explicit FileSink(std::filesystem::path path);

    bool reopen();
    const std::filesystem::path& path() const noexcept
    {
        return m_path;
    }

protected:
    void writeFormatted(std::string_view line) override;
    void flushOutput() override;

private:
    std::filesystem::path m_path;
    std::ofstream m_file;
};

} // namespace sylar::log
