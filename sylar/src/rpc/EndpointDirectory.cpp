#include "rpc/EndpointDirectory.hpp"

#include <algorithm>
#include <set>
#include <stdexcept>

#include <grpcpp/grpcpp.h>
namespace sylar::rpc
{
EndpointDirectory::EndpointDirectory(
    std::vector<std::string> endpoints,
    Options options,
    std::shared_ptr<grpc::ChannelCredentials> credentials
)
    : m_options(options)
    , m_credentials(std::move(credentials))
{
    if (!m_credentials || !options.failure_threshold ||
        !options.recovery_successes ||
        options.cooldown.count() <= 0 ||
        options.probe_timeout.count() <= 0 ||
        options.probe_timeout > std::chrono::seconds(1))
        throw std::invalid_argument("invalid endpoint policy");
    m_view.store(std::make_shared<const View>());
    update(endpoints);
    m_monitor = std::jthread([this](std::stop_token stop)
                             { probe(stop); });
}
EndpointDirectory::~EndpointDirectory()
{
    m_monitor.request_stop();
    m_condition.notify_all();
    m_monitor.join();
}
void EndpointDirectory::update(
    const std::vector<std::string>& addresses
)
{
    if (addresses.size() > 128)
        throw std::invalid_argument("too many endpoints");
    std::lock_guard update(m_updateMutex);
    auto previous = m_view.load();
    auto next = std::make_shared<View>();
    std::set<std::string> unique;
    for (const auto& address : addresses)
    {
        if (address.empty() || address.size() > 256 ||
            !unique.insert(address).second)
            throw std::invalid_argument(
                "invalid or duplicate endpoint"
            );
        auto found = std::find_if(
            previous->begin(),
            previous->end(),
            [&](const auto& item) { return item->address == address; }
        );
        if (found != previous->end())
        {
            next->push_back(*found);
            continue;
        }
        auto endpoint = std::make_shared<Endpoint>();
        endpoint->address = address;
        grpc::ChannelArguments args;
        args.SetMaxReceiveMessageSize(65536);
        args.SetMaxSendMessageSize(65536);
        args.SetInt("grpc.enable_retries", 0);
        endpoint->stub = v1::Catalog::NewStub(
            grpc::CreateCustomChannel(address, m_credentials, args)
        );
        next->push_back(std::move(endpoint));
    }
    m_view.store(std::move(next));
}
std::shared_ptr<EndpointDirectory::Endpoint>
EndpointDirectory::select(const std::string& exclude)
{
    auto view = m_view.load();
    if (view->empty())
        return {};
    auto start = m_next.fetch_add(1) % view->size();
    for (std::size_t n = 0; n < view->size(); ++n)
    {
        auto endpoint = (*view)[(start + n) % view->size()];
        std::lock_guard lock(endpoint->mutex);
        if (!endpoint->open && endpoint->address != exclude)
            return endpoint;
    }
    return {};
}
void EndpointDirectory::observe(
    const std::shared_ptr<Endpoint>& endpoint, grpc::StatusCode code
)
{
    std::lock_guard lock(endpoint->mutex);
    if (endpoint->open)
        return; // Only serialized probes can close an open circuit.
    if (code == grpc::StatusCode::UNAVAILABLE)
    {
        if (++endpoint->failures >= m_options.failure_threshold)
        {
            endpoint->open = true;
            endpoint->recovery = 0;
            endpoint->next_probe = std::chrono::steady_clock::now() +
                                   m_options.cooldown;
        }
    }
    else if (code == grpc::StatusCode::OK)
        endpoint->failures = 0;
}
std::vector<EndpointDirectory::Status>
EndpointDirectory::snapshot() const
{
    std::vector<Status> result;
    auto view = m_view.load();
    for (auto& endpoint : *view)
    {
        std::lock_guard lock(endpoint->mutex);
        result.push_back(
            {endpoint->address, endpoint->open, endpoint->failures}
        );
    }
    return result;
}
void EndpointDirectory::probe(std::stop_token stop)
{
    while (!stop.stop_requested())
    {
        auto view = m_view.load();
        for (auto& endpoint : *view)
        {
            if (stop.stop_requested())
                break;
            {
                std::lock_guard lock(endpoint->mutex);
                if (!endpoint->open || endpoint->probing ||
                    std::chrono::steady_clock::now() <
                        endpoint->next_probe)
                    continue;
                endpoint->probing = true;
            }
            grpc::ClientContext context;
            context.set_deadline(
                std::chrono::system_clock::now() +
                m_options.probe_timeout
            );
            std::stop_callback cancel(
                stop,
                [&] { context.TryCancel(); }
            );
            v1::ProductRequest request;
            request.set_product_id(1);
            v1::ProductReply reply;
            auto status = endpoint->stub
                              ->GetProduct(&context, request, &reply);
            {
                std::lock_guard lock(endpoint->mutex);
                endpoint->probing = false;
                endpoint->next_probe = std::chrono::steady_clock::now(
                                       ) +
                                       m_options.cooldown;
                endpoint->recovery = status.ok()
                                         ? endpoint->recovery + 1
                                         : 0;
                if (endpoint->recovery >=
                    m_options.recovery_successes)
                {
                    endpoint->open = false;
                    endpoint->failures = 0;
                }
            }
        }
        std::unique_lock lock(m_sleepMutex);
        m_condition.wait_for(
            lock,
            std::chrono::milliseconds(20),
            [&] { return stop.stop_requested(); }
        );
    }
}
} // namespace sylar::rpc
