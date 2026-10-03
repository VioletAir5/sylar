#include <netdb.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <latch>
#include <mutex>
#include <thread>
#include <vector>

#include <fmt/format.h>

#include "fiber/IOManagerPool.hpp"
#include "rpc/CatalogClient.hpp"
using namespace std::chrono_literals;
namespace
{
unsigned number(const char* text, unsigned max)
{
    std::string_view value(text);
    unsigned result = 0;
    auto [end, error] = std::from_chars(
        value.data(),
        value.data() + value.size(),
        result
    );
    if (error != std::errc{} || end != value.data() + value.size() ||
        !result || result > max)
        throw std::invalid_argument("invalid benchmark count");
    return result;
}
double cpuSeconds()
{
    rusage usage{};
    getrusage(RUSAGE_SELF, &usage);
    return usage.ru_utime.tv_sec + usage.ru_utime.tv_usec / 1e6 +
           usage.ru_stime.tv_sec + usage.ru_stime.tv_usec / 1e6;
}
std::size_t fdCount()
{
    std::size_t result = 0;
    for (const auto& entry :
         std::filesystem::directory_iterator("/proc/self/fd"))
    {
        (void)entry;
        ++result;
    }
    return result;
}
class HttpConnection
{
public:
    explicit HttpConnection(const std::string& endpoint)
    {
        auto colon = endpoint.rfind(':');
        if (colon == std::string::npos)
            throw std::invalid_argument(
                "HTTP endpoint requires host:port"
            );
        addrinfo hints{};
        hints.ai_socktype = SOCK_STREAM;
        addrinfo* addresses = nullptr;
        if (getaddrinfo(
                endpoint.substr(0, colon).c_str(),
                endpoint.substr(colon + 1).c_str(),
                &hints,
                &addresses
            ))
            throw std::runtime_error("resolve failed");
        for (auto* address = addresses; address;
             address = address->ai_next)
        {
            m_fd = socket(
                address->ai_family,
                SOCK_STREAM | SOCK_CLOEXEC,
                0
            );
            timeval timeout{3, 0};
            setsockopt(
                m_fd,
                SOL_SOCKET,
                SO_RCVTIMEO,
                &timeout,
                sizeof(timeout)
            );
            setsockopt(
                m_fd,
                SOL_SOCKET,
                SO_SNDTIMEO,
                &timeout,
                sizeof(timeout)
            );
            if (connect(
                    m_fd,
                    address->ai_addr,
                    address->ai_addrlen
                ) == 0)
                break;
            close(m_fd);
            m_fd = -1;
        }
        freeaddrinfo(addresses);
        if (m_fd < 0)
            throw std::runtime_error("connect failed");
    }
    ~HttpConnection()
    {
        if (m_fd >= 0)
            close(m_fd);
    }
    bool request()
    {
        const std::string
            request = "GET /products/1 HTTP/1.1\r\nHost: "
                      "benchmark\r\nConnection: keep-alive\r\n\r\n";
        std::size_t sent = 0;
        while (sent < request.size())
        {
            auto count = send(
                m_fd,
                request.data() + sent,
                request.size() - sent,
                MSG_NOSIGNAL
            );
            if (count <= 0)
                throw std::runtime_error("send failed");
            sent += count;
        }
        auto readMore = [&]
        {
            char bytes[4096];
            auto count = recv(m_fd, bytes, sizeof(bytes), 0);
            if (count <= 0)
                throw std::runtime_error("receive failed");
            m_buffer.append(bytes, count);
            if (m_buffer.size() > 1024 * 1024)
                throw std::runtime_error("response too large");
        };
        while (m_buffer.find("\r\n\r\n") == std::string::npos)
            readMore();
        auto head = m_buffer.find("\r\n\r\n");
        auto position = m_buffer.find("Content-Length:");
        if (position == std::string::npos || position > head)
            throw std::runtime_error("missing response length");
        auto length = std::stoul(m_buffer.substr(position + 15));
        auto total = head + 4 + length;
        while (m_buffer.size() < total)
            readMore();
        bool ok = m_buffer.starts_with("HTTP/1.1 200 ");
        m_buffer.erase(0, total);
        return ok;
    }

private:
    int m_fd = -1;
    std::string m_buffer;
};
} // namespace
int main(int argc, char** argv)
{
    try
    {
        if (argc != 5)
            throw std::invalid_argument(
                "usage: sylar_rpc_bench "
                "grpc-sync|fiber-eventfd|fiber-worker|http endpoint "
                "requests concurrency"
            );
        std::string mode = argv[1], endpoint = argv[2];
        if (mode != "grpc-sync" && mode != "fiber-eventfd" &&
            mode != "fiber-worker" && mode != "http")
            throw std::invalid_argument("invalid benchmark mode");
        unsigned requests = number(argv[3], 1000000),
                 concurrency = number(argv[4], 256);
        if (requests < concurrency)
            throw std::invalid_argument("requests < concurrency");
        std::atomic_bool go{false};
        std::atomic_uint ready{0};
        std::atomic_uint64_t failed{0};
        std::mutex samples_mutex;
        std::vector<double> samples;
        samples.reserve(requests);
        std::latch finished(concurrency);
        std::vector<std::jthread> threads;
        std::unique_ptr<sylar::IOManagerPool> pool;
        std::unique_ptr<sylar::rpc::GrpcRuntime> runtime;
        std::unique_ptr<sylar::rpc::CatalogClient> client;
        std::shared_ptr<grpc::Channel> channel;
        std::unique_ptr<sylar::rpc::v1::Catalog::Stub> stub;
        if (mode.starts_with("fiber"))
        {
            setenv("SYLAR_IO_DIAGNOSTICS", "1", 1);
            runtime = std::make_unique<sylar::rpc::GrpcRuntime>(
                concurrency,
                2s,
                mode == "fiber-worker"
                    ? sylar::rpc::GrpcRuntime::NotificationMode::
                          WorkerQueue
                    : sylar::rpc::GrpcRuntime::NotificationMode::
                          PerCallEventfd
            );
            client = std::make_unique<sylar::rpc::CatalogClient>(
                *runtime,
                std::vector<std::string>{endpoint}
            );
            pool = std::make_unique<sylar::IOManagerPool>(2);
        }
        if (mode == "grpc-sync")
        {
            grpc::ChannelArguments args;
            args.SetInt("grpc.enable_retries", 0);
            channel = grpc::CreateCustomChannel(
                endpoint,
                grpc::InsecureChannelCredentials(),
                args
            );
            stub = sylar::rpc::v1::Catalog::NewStub(channel);
        }
        auto syncCall = [&]
        {
            grpc::ClientContext context;
            context.set_deadline(
                std::chrono::system_clock::now() + 2s
            );
            sylar::rpc::v1::ProductRequest request;
            request.set_product_id(1);
            sylar::rpc::v1::ProductReply reply;
            return stub->GetProduct(&context, request, &reply).ok() &&
                   reply.product_id() == 1;
        };
        auto run = [&](unsigned index, sylar::IOManager* io)
        {
            std::unique_ptr<HttpConnection> http;
            auto call = [&]
            {
                if (io)
                    return client
                        ->lookup(
                            *io,
                            sylar::RequestContext::withTimeout(2s),
                            1
                        )
                        .status.ok();
                if (http)
                    return http->request();
                return syncCall();
            };
            try
            {
                if (mode == "http")
                    http = std::make_unique<HttpConnection>(endpoint);
                for (int warm = 0; warm < 10; ++warm)
                    if (!call())
                        ++failed;
            }
            catch (...)
            {
                ++failed;
            }
            ++ready;
            while (!go.load(std::memory_order_acquire))
            {
                if (io)
                    io->sleepFor(1ms);
                else
                    std::this_thread::sleep_for(1ms);
            }
            std::vector<double> local;
            for (unsigned n = index; n < requests; n += concurrency)
            {
                auto start = std::chrono::steady_clock::now();
                try
                {
                    if (!call())
                        ++failed;
                }
                catch (...)
                {
                    ++failed;
                }
                local.push_back(
                    std::chrono::duration<double, std::micro>(
                        std::chrono::steady_clock::now() - start
                    )
                        .count()
                );
            }
            {
                std::lock_guard lock(samples_mutex);
                samples.insert(
                    samples.end(),
                    local.begin(),
                    local.end()
                );
            }
            finished.count_down();
        };
        for (unsigned i = 0; i < concurrency; ++i)
        {
            if (pool)
                pool->post([&, i](sylar::IOManager& io)
                           { run(i, &io); });
            else
                threads.emplace_back([&, i] { run(i, nullptr); });
        }
        while (ready.load() != concurrency)
            std::this_thread::sleep_for(1ms);
        auto baseline_fd = fdCount();
        std::atomic_size_t peak_fd{baseline_fd};
        std::jthread sampler(
            [&](std::stop_token stop)
            {
                while (!stop.stop_requested())
                {
                    auto count = fdCount();
                    auto peak = peak_fd.load();
                    while (count > peak &&
                           !peak_fd.compare_exchange_weak(peak, count)
                    )
                    {
                    }
                    std::this_thread::sleep_for(1ms);
                }
            }
        );
        double cpu = cpuSeconds();
        auto start = std::chrono::steady_clock::now();
        go.store(true, std::memory_order_release);
        finished.wait();
        double elapsed = std::chrono::duration<double>(
                             std::chrono::steady_clock::now() - start
        )
                             .count();
        cpu = cpuSeconds() - cpu;
        sampler.request_stop();
        sampler.join();
        std::uint64_t epoll_ctl = 0, registrations = 0;
        if (pool)
        {
            for (auto& stats : pool->diagnostics())
            {
                epoll_ctl += stats.epoll_ctl_calls;
                registrations += stats.waiter_registrations;
            }
            pool->stop();
            runtime->shutdown();
        }
        threads.clear();
        std::sort(samples.begin(), samples.end());
        auto percentile = [&](double p)
        {
            return samples[static_cast<std::size_t>(
                (samples.size() - 1) * p
            )];
        };
        rusage usage{};
        getrusage(RUSAGE_SELF, &usage);
        std::cout << fmt::format(
            "{{\"mode\":\"{}\",\"requests\":{},\"concurrency\":{},"
            "\"failed\":{},\"seconds\":{},\"rps\":{},\"p50_us\":{},"
            "\"p95_us\":{},\"p99_us\":{},\"client_cpu_seconds\":{},"
            "\"client_peak_rss_kib\":{},\"client_fd_baseline\":{},"
            "\"client_fd_peak\":{},\"epoll_ctl_including_warmup\":{},"
            "\"registrations_including_warmup\":{}}}\n",
            mode,
            requests,
            concurrency,
            failed.load(),
            elapsed,
            requests / elapsed,
            percentile(.5),
            percentile(.95),
            percentile(.99),
            cpu,
            usage.ru_maxrss,
            baseline_fd,
            peak_fd.load(),
            epoll_ctl,
            registrations
        );
        return failed.load() ? 1 : 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << std::endl;
        return 1;
    }
}
