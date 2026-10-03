#pragma once
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <grpcpp/grpcpp.h>

#include "catalog.grpc.pb.h"
namespace sylar::rpc
{
class EndpointDirectory
{
public:
    struct Options
    {
        unsigned failure_threshold = 2;
        unsigned recovery_successes = 2;
        std::chrono::milliseconds cooldown{200};
        std::chrono::milliseconds probe_timeout{100};
    };
    struct Endpoint
    {
        std::string address;
        std::unique_ptr<v1::Catalog::Stub> stub;
        mutable std::mutex mutex;
        unsigned failures = 0, recovery = 0;
        bool open = false, probing = false;
        std::chrono::steady_clock::time_point next_probe{};
    };
    struct Status
    {
        std::string address;
        bool open;
        unsigned failures;
    };
    EndpointDirectory(
        std::vector<std::string> endpoints,
        Options options,
        std::shared_ptr<grpc::ChannelCredentials> credentials
    );
    ~EndpointDirectory();
    void update(const std::vector<std::string>& endpoints);
    std::shared_ptr<Endpoint> select(const std::string& exclude = {});
    void observe(
        const std::shared_ptr<Endpoint>& endpoint,
        grpc::StatusCode code
    );
    std::vector<Status> snapshot() const;

private:
    void probe(std::stop_token stop);
    using View = std::vector<std::shared_ptr<Endpoint>>;
    Options m_options;
    std::shared_ptr<grpc::ChannelCredentials> m_credentials;
    std::atomic<std::shared_ptr<const View>> m_view;
    std::atomic_size_t m_next{0};
    std::mutex m_updateMutex;
    std::mutex m_sleepMutex;
    std::condition_variable m_condition;
    std::jthread m_monitor;
};
} // namespace sylar::rpc
