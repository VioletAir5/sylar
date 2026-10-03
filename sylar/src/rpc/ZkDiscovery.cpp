#include "rpc/ZkDiscovery.hpp"

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>

#include "rpc/GrpcRuntime.hpp"
#include "zk/ZkClient.hpp"
namespace sylar::rpc
{
struct ZkDiscovery::Impl
{
    std::string hosts, root, instance, endpoint;
    Publish publish;
    std::atomic_uint64_t errors{0};
    std::atomic_bool healthy{false};
    std::mutex mutex;
    std::condition_variable condition;
    std::jthread thread, expiry;
    std::mutex publication;
    std::chrono::steady_clock::time_point
        last_success = std::chrono::steady_clock::now();
    ~Impl()
    {
        thread.request_stop();
        expiry.request_stop();
        condition.notify_all();
        if (thread.joinable())
            thread.join();
        if (expiry.joinable())
            expiry.join();
    }
    void run(std::stop_token stop)
    {
        using namespace std::chrono_literals;
        std::optional<ZkClient> client;
        auto refreshed = std::chrono::steady_clock::now();
        bool stale_published = false;
        while (!stop.stop_requested())
        {
            try
            {
                if (client && client->expired())
                    client.reset();
                if (!client)
                    client.emplace(ZkClient::connect(hosts, 1000ms));
                if (!client->connected())
                    throw std::runtime_error("registry disconnected");
                if (!publish)
                {
                    std::size_t end = 1;
                    while ((end = root.find('/', end)) !=
                           std::string::npos)
                    {
                        auto path = root.substr(0, end);
                        if (!client->exists(path))
                        {
                            try
                            {
                                client->create(path, "");
                            }
                            catch (...)
                            {
                                if (!client->exists(path))
                                    throw;
                            }
                        }
                        ++end;
                    }
                    if (!client->exists(root))
                    {
                        try
                        {
                            client->create(root, "");
                        }
                        catch (...)
                        {
                            if (!client->exists(root))
                                throw;
                        }
                    }
                    auto path = root + "/" + instance;
                    if (!client->exists(path))
                        client->create(path, endpoint, true);
                    // A duplicate instance must never overwrite
                    // another session's node.
                    else if (!client->ownsEphemeral(path) || client->get(path) != endpoint)
                        throw std::runtime_error(
                            "instance identity collision"
                        );
                }
                else
                {
                    std::set<std::string> addresses;
                    if (client->exists(root))
                        for (auto& child : client->children(root))
                        {
                            const auto path = root + "/" + child;
                            try
                            {
                                auto value = client->get(path);
                                if (value.empty() ||
                                    value.size() > 256)
                                    throw std::runtime_error(
                                        "bad endpoint value"
                                    );
                                addresses.insert(std::move(value));
                            }
                            catch (...)
                            {
                                if (client->exists(path))
                                    throw;
                                // A node removed between children/get
                                // is simply absent.
                            }
                        }
                    if (!client->connected())
                        throw std::runtime_error(
                            "registry disconnected during refresh"
                        );
                    std::lock_guard lock(publication);
                    publish(std::vector<std::string>(
                        addresses.begin(),
                        addresses.end()
                    ));
                    last_success = std::chrono::steady_clock::now();
                }
                refreshed = std::chrono::steady_clock::now();
                stale_published = false;
                healthy.store(true);
            }
            catch (...)
            {
                ++errors;
                healthy.store(false);
                if (publish && !stale_published &&
                    std::chrono::steady_clock::now() - refreshed >=
                        1000ms)
                {
                    try
                    {
                        std::lock_guard lock(publication);
                        publish({});
                        stale_published = true;
                    }
                    catch (...)
                    {
                    }
                }
            }
            std::unique_lock lock(mutex);
            condition.wait_for(
                lock,
                100ms,
                [&] { return stop.stop_requested(); }
            );
        }
    }
};
namespace
{
void validate(const std::string& hosts, const std::string& root)
{
    if (hosts.empty() || root.size() < 2 || root.front() != '/' ||
        root.back() == '/' || root.find("//") != std::string::npos ||
        root.find('\0') != std::string::npos)
        throw std::invalid_argument("invalid registry configuration");
}
} // namespace
ZkDiscovery::ZkDiscovery(std::unique_ptr<Impl> impl)
    : m_impl(std::move(impl))
{
    if (m_impl->publish)
        m_impl->expiry = std::jthread(
            [this](std::stop_token stop)
            {
                while (!stop.stop_requested())
                {
                    {
                        std::lock_guard lock(m_impl->publication);
                        if (std::chrono::steady_clock::now() -
                                m_impl->last_success >
                            std::chrono::seconds(1))
                        {
                            try
                            {
                                m_impl->publish({});
                                m_impl->healthy.store(false);
                            }
                            catch (...)
                            {
                                ++m_impl->errors;
                            }
                        }
                    }
                    std::unique_lock lock(m_impl->mutex);
                    m_impl->condition.wait_for(
                        lock,
                        std::chrono::milliseconds(50),
                        [&] { return stop.stop_requested(); }
                    );
                }
            }
        );
    m_impl->thread = std::jthread([this](std::stop_token stop)
                                  { m_impl->run(stop); });
}
std::unique_ptr<ZkDiscovery> ZkDiscovery::watch(
    std::string hosts, std::string root, Publish publish
)
{
    validate(hosts, root);
    if (!publish)
        throw std::invalid_argument("empty discovery callback");
    auto impl = std::make_unique<Impl>();
    impl->hosts = std::move(hosts);
    impl->root = std::move(root);
    impl->publish = std::move(publish);
    return std::unique_ptr<ZkDiscovery>(new ZkDiscovery(std::move(impl
    )));
}
std::unique_ptr<ZkDiscovery> ZkDiscovery::advertise(
    std::string hosts,
    std::string root,
    std::string instance,
    std::string endpoint
)
{
    validate(hosts, root);
    if (instance.empty() || !GrpcRuntime::validRequestId(instance) ||
        endpoint.empty() || endpoint.size() > 256)
        throw std::invalid_argument("invalid advertised endpoint");
    auto impl = std::make_unique<Impl>();
    impl->hosts = std::move(hosts);
    impl->root = std::move(root);
    impl->instance = std::move(instance);
    impl->endpoint = std::move(endpoint);
    return std::unique_ptr<ZkDiscovery>(new ZkDiscovery(std::move(impl
    )));
}
ZkDiscovery::~ZkDiscovery()
{
    m_impl->thread.request_stop();
    m_impl->expiry.request_stop();
    m_impl->condition.notify_all();
    m_impl->thread.join();
    if (m_impl->expiry.joinable())
        m_impl->expiry.join();
}
std::uint64_t ZkDiscovery::errors() const noexcept
{
    return m_impl->errors.load();
}
bool ZkDiscovery::healthy() const noexcept
{
    return m_impl->healthy.load();
}
} // namespace sylar::rpc
