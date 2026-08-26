#pragma once
// Per-core counters and connection-latency histogram, aggregated on read.
// docs/SPEC.md §5.6 (DP-U-15/16/17).
//
// Contract:
//  - Each EventLoop owns its own counters (accepted, active, bytes each
//    direction, connect failures, per-backend connection counts) with no
//    cross-loop writes.
//  - Aggregation happens when something reads the stats (e.g. the admin
//    socket), not on every write — the hot path only ever increments a
//    local counter (DP-U-16).
//  - Connection establishment latency goes into an HDR histogram; report
//    p50/p95/p99/p99.9 (DP-U-17).
//
// Implemented in src/stats.cpp with a small self-contained log-linear
// histogram (16 sub-buckets per octave, ~6% quantile error) rather than a
// vendored dependency, so the build stays hermetic. Buckets are relaxed
// atomics: recording is a single fetch_add off any lock, and percentile
// reads walk the buckets without disturbing the writers.

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace sluiced {

struct PerCoreCounters {
    std::uint64_t accepted_connections = 0;
    std::uint64_t active_connections = 0;
    std::uint64_t bytes_client_to_upstream = 0;
    std::uint64_t bytes_upstream_to_client = 0;
    std::uint64_t connect_failures = 0;
};

struct LatencyPercentiles {
    double p50 = 0;
    double p95 = 0;
    double p99 = 0;
    double p999 = 0;
};

class StatsAggregator {
public:
    // Called by the admin socket handler; walks every EventLoop's counters
    // and sums them. Not on the hot path.
    [[nodiscard]] PerCoreCounters Aggregate(const std::vector<const PerCoreCounters*>& per_core) const;

    [[nodiscard]] LatencyPercentiles ConnectLatencyPercentiles() const;

    // Called from the connection-establishment path; must be cheap.
    void RecordConnectLatency(std::uint64_t micros) noexcept;

    // Largest connect latency seen, in microseconds (0 if none recorded).
    [[nodiscard]] std::uint64_t MaxConnectLatencyMicros() const noexcept;

private:
    // 34 octaves x 16 sub-buckets covers 0 .. ~2^34 us (~4.7 hours), far past
    // any latency we would ever record; the tail is clamped into the top bucket.
    static constexpr std::size_t kBuckets = 34 * 16;
    std::array<std::atomic<std::uint64_t>, kBuckets> buckets_{};
    std::atomic<std::uint64_t> count_{0};
    std::atomic<std::uint64_t> max_micros_{0};
};

}  // namespace sluiced
