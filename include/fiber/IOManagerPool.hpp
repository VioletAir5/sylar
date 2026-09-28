#pragma once

#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

#include "fiber/IOManager.hpp"

namespace sylar
{

// A fixed set of owner-thread IOManagers. Each callback and its
// Fibers stay on one worker; post() may be called from another
// thread.
class IOManagerPool
{
public:
    explicit IOManagerPool(std::size_t workers);
    ~IOManagerPool();

    IOManagerPool(const IOManagerPool&) = delete;
    IOManagerPool& operator=(const IOManagerPool&) = delete;
    IOManagerPool(IOManagerPool&&) = delete;
    IOManagerPool& operator=(IOManagerPool&&) = delete;

    void post(std::function<void(IOManager&)> callback);
    void stop();
    std::size_t size() const noexcept;

private:
    struct Worker;

    std::mutex m_mutex;
    std::vector<std::unique_ptr<Worker>> m_workers;
    std::size_t m_next = 0;
    bool m_stopped = false;
};

} // namespace sylar
