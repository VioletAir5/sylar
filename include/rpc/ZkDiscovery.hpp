#pragma once
#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>
namespace sylar::rpc
{
// Polling discovery runs on its own thread; expired sessions are
// rebuilt. Empty view is published on stale data, never a partial
// failed refresh.
class ZkDiscovery
{
public:
    using Publish = std::function<
        void(const std::vector<std::string>&)>;
    static std::unique_ptr<ZkDiscovery>
    watch(std::string hosts, std::string root, Publish publish);
    static std::unique_ptr<ZkDiscovery> advertise(
        std::string hosts,
        std::string root,
        std::string instance,
        std::string endpoint
    );
    ~ZkDiscovery();
    std::uint64_t errors() const noexcept;
    bool healthy() const noexcept;

private:
    struct Impl;
    explicit ZkDiscovery(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> m_impl;
};
} // namespace sylar::rpc
