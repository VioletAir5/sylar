#pragma once

#include <chrono>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace sylar
{

class ZkClient
{
public:
    static ZkClient connect(
        const std::string& hosts,
        std::chrono::milliseconds timeout = std::chrono::seconds(5)
    );

    ~ZkClient();
    ZkClient(const ZkClient&) = delete;
    ZkClient& operator=(const ZkClient&) = delete;
    ZkClient(ZkClient&&) noexcept;
    ZkClient& operator=(ZkClient&&) noexcept;

    void create(
        const std::string& path,
        std::string_view value,
        bool ephemeral = false
    );
    bool exists(const std::string& path);
    std::string get(const std::string& path);
    void
    set(const std::string& path,
        std::string_view value,
        int expected_version = -1);
    void erase(const std::string& path, int expected_version = -1);
    std::vector<std::string> children(const std::string& path);

private:
    struct Impl;
    explicit ZkClient(std::unique_ptr<Impl> impl) noexcept;

    std::unique_ptr<Impl> m_impl;
};

} // namespace sylar