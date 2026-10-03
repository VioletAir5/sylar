#pragma once
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
namespace sylar::rpc
{
struct TraceContext
{
    std::string trace_id, span_id, flags = "01";
    static TraceContext root();
    static std::optional<TraceContext> parse(const std::string& parent
    );
    TraceContext child() const;
    std::string header() const;
    bool sampled() const;
};
struct SpanRecord
{
    std::string trace_id, span_id, parent_span_id, name, kind;
    std::uint64_t start_unix_ns = 0, duration_us = 0,
                  owner_queue_us = 0;
    int status = 0;
};
class TraceRecorder
{
public:
    explicit TraceRecorder(std::size_t capacity = 4096);
    void record(SpanRecord record);
    std::string json() const;
    std::size_t size() const;
    std::uint64_t dropped() const;

private:
    std::size_t m_capacity;
    mutable std::mutex m_mutex;
    std::deque<SpanRecord> m_spans;
    std::uint64_t m_dropped = 0;
};
class TraceScope
{
public:
    TraceScope(
        std::shared_ptr<TraceRecorder> recorder,
        const std::string& parent,
        std::string name,
        std::string kind
    );
    ~TraceScope();
    const std::string& header() const;
    void finish(int status, std::uint64_t owner_queue_us = 0);

private:
    std::shared_ptr<TraceRecorder> m_recorder;
    SpanRecord m_record;
    std::string m_header;
    std::chrono::steady_clock::time_point m_started;
    bool m_finished = false, m_sampled = false;
};
} // namespace sylar::rpc
