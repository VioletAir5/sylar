#pragma once

#include <pthread.h>
#include <signal.h>
#include <sys/signalfd.h>
#include <unistd.h>

#include <cerrno>
#include <system_error>

namespace sylar
{

class SignalFd
{
public:
    SignalFd()
    {
        sigset_t mask;
        ::sigemptyset(&mask);
        ::sigaddset(&mask, SIGINT);
        ::sigaddset(&mask, SIGTERM);
        const int error = ::pthread_sigmask(
            SIG_BLOCK,
            &mask,
            &m_previous
        );
        if (error != 0)
        {
            throw std::system_error(
                error,
                std::generic_category(),
                "pthread_sigmask"
            );
        }
        m_fd = ::signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC);
        if (m_fd < 0)
        {
            const int saved = errno;
            ::pthread_sigmask(SIG_SETMASK, &m_previous, nullptr);
            throw std::system_error(
                saved,
                std::generic_category(),
                "signalfd"
            );
        }
    }

    ~SignalFd()
    {
        ::close(m_fd);
        ::pthread_sigmask(SIG_SETMASK, &m_previous, nullptr);
    }

    SignalFd(const SignalFd&) = delete;
    SignalFd& operator=(const SignalFd&) = delete;

    int get() const noexcept
    {
        return m_fd;
    }

private:
    int m_fd = -1;
    sigset_t m_previous{};
};

} // namespace sylar
