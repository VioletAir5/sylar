#include "rpc/Trace.hpp"

#include <algorithm>
#include <array>
#include <stdexcept>
#include <vector>

#include <fmt/format.h>
#include <openssl/rand.h>
namespace sylar::rpc
{
namespace
{
std::string randomHex(std::size_t size)
{
    std::array<unsigned char, 16> bytes{};
    if (RAND_bytes(bytes.data(), static_cast<int>(size)) != 1)
        throw std::runtime_error("trace random failed");
    static const char* hex = "0123456789abcdef";
    std::string result;
    for (std::size_t i = 0; i < size; ++i)
    {
        result += hex[bytes[i] >> 4];
        result += hex[bytes[i] & 15];
    }
    if (result.find_first_not_of('0') == std::string::npos)
        result.back() = '1';
    return result;
}
bool hex(const std::string& text, bool nonzero)
{
    return std::all_of(
               text.begin(),
               text.end(),
               [](char c) {
                   return (c >= '0' && c <= '9') ||
                          (c >= 'a' && c <= 'f');
               }
           ) &&
           (!nonzero ||
            text.find_first_not_of('0') != std::string::npos);
}
std::string quote(const std::string& text)
{
    std::string result = "\"";
    for (unsigned char c : text)
    {
        if (c == '"' || c == '\\')
        {
            result += '\\';
            result += c;
        }
        else if (c < 32)
            result += fmt::format("\\u{:04x}", c);
        else
            result += c;
    }
    return result + '"';
}
} // namespace
TraceContext TraceContext::root()
{
    return {randomHex(16), randomHex(8), "01"};
}
std::optional<TraceContext>
TraceContext::parse(const std::string& text)
{
    if (text.empty())
        return {};
    if (text.size() != 55 || text.substr(0, 3) != "00-" ||
        text[35] != '-' || text[52] != '-')
        throw std::invalid_argument(
            "invalid traceparent version or size"
        );
    TraceContext result{
        text.substr(3, 32),
        text.substr(36, 16),
        text.substr(53, 2)};
    if (!hex(result.trace_id, true) || !hex(result.span_id, true) ||
        !hex(result.flags, false))
        throw std::invalid_argument("invalid traceparent ID");
    return result;
}
TraceContext TraceContext::child() const
{
    return {trace_id, randomHex(8), flags};
}
std::string TraceContext::header() const
{
    // Wire size is known before encoding (55 bytes for v00).
    // One reservation avoids growth of intermediate concatenations.
    std::string result;
    result.reserve(
        5 + trace_id.size() + span_id.size() + flags.size()
    );
    result.append("00-");
    result.append(trace_id);
    result.push_back('-');
    result.append(span_id);
    result.push_back('-');
    result.append(flags);
    return result;
}
bool TraceContext::sampled() const
{
    const char value = flags.back();
    unsigned digit = value <= '9' ? value - '0' : value - 'a' + 10;
    return digit & 1;
}
TraceRecorder::TraceRecorder(std::size_t capacity)
    : m_capacity(capacity)
{
    if (capacity > 1000000)
        throw std::invalid_argument("trace capacity too large");
}
void TraceRecorder::record(SpanRecord record)
{
    auto entry = m_capacity ? std::make_shared<const SpanRecord>(
                                  std::move(record)
                              )
                            : nullptr;
    std::lock_guard lock(m_mutex);
    if (m_capacity == 0)
    {
        ++m_dropped;
        return;
    }
    if (m_spans.size() >= m_capacity)
    {
        ++m_dropped;
        m_spans.pop_front();
    }
    m_spans.push_back(std::move(entry));
}
std::size_t TraceRecorder::size() const
{
    std::lock_guard lock(m_mutex);
    return m_spans.size();
}
std::uint64_t TraceRecorder::dropped() const
{
    std::lock_guard lock(m_mutex);
    return m_dropped;
}
std::string TraceRecorder::json() const
{
    // Pin immutable records under the mutex, then encode outside it.
    // Eviction cannot invalidate an export already in progress.
    std::vector<std::shared_ptr<const SpanRecord>> snapshot;
    std::uint64_t dropped;
    {
        std::lock_guard lock(m_mutex);
        snapshot.assign(m_spans.begin(), m_spans.end());
        dropped = m_dropped;
    }
    std::string result = "{\"dropped\":" + std::to_string(dropped) +
                         ",\"spans\":[";
    bool first = true;
    for (const auto& entry : snapshot)
    {
        const auto& span = *entry;
        if (!first)
            result += ',';
        first = false;
        result += fmt::format(
            "{{\"trace_id\":{},\"span_id\":{},\"parent_span_id\":{},"
            "\"name\":{},\"kind\":{},\"start_unix_ns\":{},\"duration_"
            "us\":{},\"owner_queue_us\":{},\"status_code\":{}}}",
            quote(span.trace_id),
            quote(span.span_id),
            quote(span.parent_span_id),
            quote(span.name),
            quote(span.kind),
            span.start_unix_ns,
            span.duration_us,
            span.owner_queue_us,
            span.status
        );
    }
    return result + "]}\n";
}
TraceScope::TraceScope(
    std::shared_ptr<TraceRecorder> recorder,
    const std::string& parent,
    std::string name,
    std::string kind
)
    : m_recorder(std::move(recorder))
    , m_started(std::chrono::steady_clock::now())
{
    auto parsed = TraceContext::parse(parent);
    auto current = parsed ? parsed->child() : TraceContext::root();
    m_sampled = current.sampled();
    m_header = current.header();
    m_record = {
        current.trace_id,
        current.span_id,
        parsed ? parsed->span_id : "",
        std::move(name),
        std::move(kind),
        static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::system_clock::now().time_since_epoch()
            )
                .count()
        ),
        0,
        0,
        0};
}
TraceScope::~TraceScope()
{
    try
    {
        if (!m_finished)
            finish(-1);
    }
    catch (...)
    {
    }
}
const std::string& TraceScope::header() const
{
    return m_header;
}
void TraceScope::finish(int status, std::uint64_t queue)
{
    if (m_finished)
        return;
    m_finished = true;
    m_record.status = status;
    m_record.owner_queue_us = queue;
    m_record.duration_us = std::chrono::duration_cast<
                               std::chrono::microseconds>(
                               std::chrono::steady_clock::now() -
                               m_started
    )
                               .count();
    if (m_recorder && m_sampled)
        m_recorder->record(std::move(m_record));
}
} // namespace sylar::rpc
