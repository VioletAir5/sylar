#include "fiber/Hook.hpp"

#include <fcntl.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <limits>
#include <stdexcept>
#include <vector>

#include "fiber/Fiber.hpp"
#include "fiber/IOManager.hpp"
#include "fiber/Scheduler.hpp"

namespace sylar
{
namespace
{

thread_local std::vector<const HookScope*> hook_scopes;
thread_local std::chrono::milliseconds
    hook_timeout = std::chrono::seconds(30);

IOManager* activeIo() noexcept
{
    if (hook_scopes.empty())
    {
        return nullptr;
    }
    auto* io = IOManager::GetThis();
    auto* fiber = Fiber::GetCurrent();
    return io && Scheduler::GetThis() && fiber && fiber->getId() != 0
               ? io
               : nullptr;
}

IOManager* socketIo(int fd) noexcept
{
    auto* io = activeIo();
    if (!io)
    {
        return nullptr;
    }
    const int flags = ::fcntl(fd, F_GETFL);
    if (flags < 0 || (flags & O_NONBLOCK) == 0)
    {
        return nullptr;
    }
    int type = 0;
    socklen_t length = sizeof(type);
    return ::getsockopt(fd, SOL_SOCKET, SO_TYPE, &type, &length) == 0
               ? io
               : nullptr;
}

bool wait(IOManager& io, int fd, IOManager::Event event) noexcept
{
    try
    {
        const auto result = event == IOManager::Event::Read
                                ? io.waitReadable(fd, hook_timeout)
                                : io.waitWritable(fd, hook_timeout);
        if (result == IOManager::WaitResult::Ready)
        {
            return true;
        }
        errno = result == IOManager::WaitResult::TimedOut ? ETIMEDOUT
                                                          : ECANCELED;
    }
    catch (...)
    {
        errno = EIO;
    }
    return false;
}

template <typename Call>
ssize_t
socketCall(int fd, IOManager::Event event, Call&& call) noexcept
{
    auto* io = socketIo(fd);
    for (;;)
    {
        const auto result = static_cast<ssize_t>(call());
        if (result >= 0 || errno != EAGAIN || !io)
        {
            return result;
        }
        if (!wait(*io, fd, event))
        {
            return -1;
        }
    }
}

int hookedAccept(
    int fd, sockaddr* address, socklen_t* length, int flags
) noexcept
{
    auto* io = socketIo(fd);
    const int effective_flags = io ? flags | SOCK_NONBLOCK |
                                         SOCK_CLOEXEC
                                   : flags;
    for (;;)
    {
        const int client = static_cast<int>(::syscall(
            SYS_accept4,
            fd,
            address,
            length,
            effective_flags
        ));
        if (client >= 0 || errno != EAGAIN || !io)
        {
            return client;
        }
        if (!wait(*io, fd, IOManager::Event::Read))
        {
            return -1;
        }
    }
}

int hookedSleep(
    const timespec* requested, timespec* remaining
) noexcept
{
    if (!requested || requested->tv_sec < 0 ||
        requested->tv_nsec < 0 || requested->tv_nsec >= 1000000000)
    {
        errno = EINVAL;
        return -1;
    }
    auto* io = activeIo();
    if (!io)
    {
        return static_cast<int>(
            ::syscall(SYS_nanosleep, requested, remaining)
        );
    }
    const auto duration = std::chrono::seconds(requested->tv_sec) +
                          std::chrono::nanoseconds(requested->tv_nsec
                          );
    const auto delay = std::chrono::ceil<std::chrono::milliseconds>(
        duration
    );
    try
    {
        io->sleepFor(delay);
        if (remaining)
        {
            *remaining = {};
        }
        return 0;
    }
    catch (...)
    {
        errno = EIO;
        return -1;
    }
}

} // namespace

HookScope::HookScope(std::chrono::milliseconds timeout)
    : m_timeout(timeout)
{
    if (timeout.count() < 0)
    {
        throw std::invalid_argument("negative hook timeout");
    }
    hook_scopes.push_back(this);
    hook_timeout = m_timeout;
}

HookScope::~HookScope()
{
    const auto found = std::find(
        hook_scopes.begin(),
        hook_scopes.end(),
        this
    );
    if (found != hook_scopes.end())
    {
        hook_scopes.erase(found);
    }
    hook_timeout = hook_scopes.empty()
                       ? std::chrono::seconds(30)
                       : hook_scopes.back()->m_timeout;
}

bool HookScope::enabled() noexcept
{
    return !hook_scopes.empty();
}

} // namespace sylar

extern "C" ssize_t read(int fd, void* buffer, size_t count)
{
    return sylar::socketCall(
        fd,
        sylar::IOManager::Event::Read,
        [&] { return ::syscall(SYS_read, fd, buffer, count); }
    );
}

extern "C" ssize_t write(int fd, const void* buffer, size_t count)
{
    return sylar::socketCall(
        fd,
        sylar::IOManager::Event::Write,
        [&] { return ::syscall(SYS_write, fd, buffer, count); }
    );
}

extern "C" ssize_t recv(int fd, void* buffer, size_t count, int flags)
{
    return sylar::socketCall(
        fd,
        sylar::IOManager::Event::Read,
        [&]
        {
            return ::syscall(
                SYS_recvfrom,
                fd,
                buffer,
                count,
                flags,
                nullptr,
                nullptr
            );
        }
    );
}

extern "C" ssize_t
send(int fd, const void* buffer, size_t count, int flags)
{
    return sylar::socketCall(
        fd,
        sylar::IOManager::Event::Write,
        [&]
        {
            return ::syscall(
                SYS_sendto,
                fd,
                buffer,
                count,
                flags,
                nullptr,
                0
            );
        }
    );
}

extern "C" ssize_t readv(int fd, const iovec* buffers, int count)
{
    return sylar::socketCall(
        fd,
        sylar::IOManager::Event::Read,
        [&] { return ::syscall(SYS_readv, fd, buffers, count); }
    );
}

extern "C" ssize_t writev(int fd, const iovec* buffers, int count)
{
    return sylar::socketCall(
        fd,
        sylar::IOManager::Event::Write,
        [&] { return ::syscall(SYS_writev, fd, buffers, count); }
    );
}

extern "C" ssize_t recvfrom(
    int fd,
    void* buffer,
    size_t count,
    int flags,
    sockaddr* source,
    socklen_t* source_length
)
{
    return sylar::socketCall(
        fd,
        sylar::IOManager::Event::Read,
        [&]
        {
            return ::syscall(
                SYS_recvfrom,
                fd,
                buffer,
                count,
                flags,
                source,
                source_length
            );
        }
    );
}

extern "C" ssize_t sendto(
    int fd,
    const void* buffer,
    size_t count,
    int flags,
    const sockaddr* destination,
    socklen_t destination_length
)
{
    return sylar::socketCall(
        fd,
        sylar::IOManager::Event::Write,
        [&]
        {
            return ::syscall(
                SYS_sendto,
                fd,
                buffer,
                count,
                flags,
                destination,
                destination_length
            );
        }
    );
}

extern "C" ssize_t recvmsg(int fd, msghdr* message, int flags)
{
    return sylar::socketCall(
        fd,
        sylar::IOManager::Event::Read,
        [&] { return ::syscall(SYS_recvmsg, fd, message, flags); }
    );
}

extern "C" ssize_t sendmsg(int fd, const msghdr* message, int flags)
{
    return sylar::socketCall(
        fd,
        sylar::IOManager::Event::Write,
        [&] { return ::syscall(SYS_sendmsg, fd, message, flags); }
    );
}

extern "C" int close(int fd)
{
    if (auto* io = sylar::activeIo())
    {
        try
        {
            io->cancel(fd, sylar::IOManager::Event::Read);
            io->cancel(fd, sylar::IOManager::Event::Write);
        }
        catch (...)
        {
        }
    }
    return static_cast<int>(::syscall(SYS_close, fd));
}

extern "C" int
connect(int fd, const sockaddr* address, socklen_t length)
{
    auto* io = sylar::socketIo(fd);
    const int result = static_cast<int>(
        ::syscall(SYS_connect, fd, address, length)
    );
    if (result == 0 || errno != EINPROGRESS || !io)
    {
        return result;
    }
    if (!sylar::wait(*io, fd, sylar::IOManager::Event::Write))
    {
        return -1;
    }
    int error = 0;
    socklen_t error_length = sizeof(error);
    if (::getsockopt(
            fd,
            SOL_SOCKET,
            SO_ERROR,
            &error,
            &error_length
        ) < 0)
    {
        return -1;
    }
    if (error != 0)
    {
        errno = error;
        return -1;
    }
    return 0;
}

extern "C" int
accept4(int fd, sockaddr* address, socklen_t* length, int flags)
{
    return sylar::hookedAccept(fd, address, length, flags);
}

extern "C" int accept(int fd, sockaddr* address, socklen_t* length)
{
    return sylar::hookedAccept(fd, address, length, 0);
}

extern "C" int
nanosleep(const timespec* requested, timespec* remaining)
{
    return sylar::hookedSleep(requested, remaining);
}

extern "C" int usleep(useconds_t microseconds)
{
    const timespec requested{
        static_cast<time_t>(microseconds / 1000000),
        static_cast<long>(microseconds % 1000000) * 1000};
    return sylar::hookedSleep(&requested, nullptr);
}

extern "C" unsigned int sleep(unsigned int seconds)
{
    const timespec requested{static_cast<time_t>(seconds), 0};
    return sylar::hookedSleep(&requested, nullptr) == 0 ? 0 : seconds;
}
