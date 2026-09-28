#include "zk/ZkClient.hpp"

#include <algorithm>
#include <condition_variable>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <utility>

#include <zookeeper/zookeeper.h>

namespace sylar
{
namespace
{

void checkPath(const std::string& path)
{
    if (path.empty() || path.front() != '/' ||
        path.find(char{}) != std::string::npos)
    {
        throw std::invalid_argument("invalid ZooKeeper path");
    }
}

void checkValue(std::string_view value)
{
    if (value.size() > 1024 * 1024 ||
        value.size() >
            static_cast<std::size_t>(std::numeric_limits<int>::max()))
    {
        throw std::length_error("ZooKeeper value too large");
    }
}

void checkResult(int result, const char* operation)
{
    if (result != ZOK)
    {
        throw std::runtime_error(
            std::string(operation) + ": " + ::zerror(result)
        );
    }
}

struct ConnectionState
{
    std::mutex mutex;
    std::condition_variable condition;
    bool connected = false;
    bool terminal = false;
};

void watcher(
    zhandle_t*, int type, int state, const char*, void* context
)
{
    if (type != ZOO_SESSION_EVENT || !context)
    {
        return;
    }
    auto& session = *static_cast<ConnectionState*>(context);
    {
        std::lock_guard lock(session.mutex);
        session.connected = state == ZOO_CONNECTED_STATE;
        session.terminal = state == ZOO_EXPIRED_SESSION_STATE ||
                           state == ZOO_AUTH_FAILED_STATE;
    }
    session.condition.notify_all();
}

} // namespace

struct ZkClient::Impl
{
    ~Impl()
    {
        if (handle)
        {
            ::zookeeper_close(handle);
        }
    }

    ConnectionState session;
    zhandle_t* handle = nullptr;
};

ZkClient::ZkClient(std::unique_ptr<Impl> impl) noexcept
    : m_impl(std::move(impl))
{
}

ZkClient::~ZkClient() = default;
ZkClient::ZkClient(ZkClient&&) noexcept = default;
ZkClient& ZkClient::operator=(ZkClient&&) noexcept = default;

ZkClient ZkClient::connect(
    const std::string& hosts, std::chrono::milliseconds timeout
)
{
    if (hosts.empty() || hosts.find(char{}) != std::string::npos ||
        timeout.count() <= 0 ||
        timeout.count() > std::numeric_limits<int>::max())
    {
        throw std::invalid_argument(
            "invalid ZooKeeper connection options"
        );
    }
    auto impl = std::make_unique<Impl>();
    impl->handle = ::zookeeper_init(
        hosts.c_str(),
        watcher,
        static_cast<int>(timeout.count()),
        nullptr,
        &impl->session,
        0
    );
    if (!impl->handle)
    {
        throw std::runtime_error("zookeeper_init failed");
    }
    std::unique_lock lock(impl->session.mutex);
    const bool signalled = impl->session.condition.wait_for(
        lock,
        timeout,
        [&]
        { return impl->session.connected || impl->session.terminal; }
    );
    if (!signalled || !impl->session.connected)
    {
        throw std::runtime_error("ZooKeeper connection timed out "
                                 "or session failed");
    }
    lock.unlock();
    return ZkClient(std::move(impl));
}

void ZkClient::create(
    const std::string& path, std::string_view value, bool ephemeral
)
{
    checkPath(path);
    checkValue(value);
    checkResult(
        ::zoo_create(
            m_impl->handle,
            path.c_str(),
            value.data(),
            static_cast<int>(value.size()),
            &ZOO_OPEN_ACL_UNSAFE,
            ephemeral ? ZOO_EPHEMERAL : 0,
            nullptr,
            0
        ),
        "zoo_create"
    );
}

bool ZkClient::exists(const std::string& path)
{
    checkPath(path);
    Stat stat{};
    const auto
        result = ::zoo_exists(m_impl->handle, path.c_str(), 0, &stat);
    if (result == ZNONODE)
    {
        return false;
    }
    checkResult(result, "zoo_exists");
    return true;
}

std::string ZkClient::get(const std::string& path)
{
    checkPath(path);
    std::string value(1024 * 1024, char{});
    int length = static_cast<int>(value.size());
    Stat stat{};
    checkResult(
        ::zoo_get(
            m_impl->handle,
            path.c_str(),
            0,
            value.data(),
            &length,
            &stat
        ),
        "zoo_get"
    );
    if (length < 0 || length > static_cast<int>(value.size()))
    {
        throw std::runtime_error("invalid ZooKeeper data length");
    }
    value.resize(static_cast<std::size_t>(length));
    return value;
}

void ZkClient::set(
    const std::string& path,
    std::string_view value,
    int expected_version
)
{
    checkPath(path);
    checkValue(value);
    checkResult(
        ::zoo_set(
            m_impl->handle,
            path.c_str(),
            value.data(),
            static_cast<int>(value.size()),
            expected_version
        ),
        "zoo_set"
    );
}

void ZkClient::erase(const std::string& path, int expected_version)
{
    checkPath(path);
    checkResult(
        ::zoo_delete(m_impl->handle, path.c_str(), expected_version),
        "zoo_delete"
    );
}

std::vector<std::string> ZkClient::children(const std::string& path)
{
    checkPath(path);
    String_vector raw{};
    checkResult(
        ::zoo_get_children(m_impl->handle, path.c_str(), 0, &raw),
        "zoo_get_children"
    );
    std::vector<std::string> result;
    try
    {
        result.reserve(raw.count);
        for (int i = 0; i < raw.count; ++i)
        {
            result.emplace_back(raw.data[i]);
        }
    }
    catch (...)
    {
        ::deallocate_String_vector(&raw);
        throw;
    }
    ::deallocate_String_vector(&raw);
    return result;
}

} // namespace sylar