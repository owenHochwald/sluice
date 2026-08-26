#include "sluiced/stats.hpp"

namespace sluiced {

namespace {

// Log-linear bucketing: values below 16 land in their own exact bucket; above
// that, each octave (power-of-two range) is split into 16 sub-buckets keyed on
// the 4 bits just under the most significant one. Monotonic in the value.
std::size_t BucketOf(std::uint64_t v) noexcept {
  if (v < 16) return static_cast<std::size_t>(v);
  const int msb = 63 - __builtin_clzll(v);
  const int shift = msb - 4;
  const std::uint64_t mant = (v >> shift) & 0xF;
  const std::size_t base = static_cast<std::size_t>(msb - 4 + 1) * 16;
  return base + static_cast<std::size_t>(mant);
}

// Lower bound of the value range a bucket represents; used as the reported
// value at a percentile.
std::uint64_t ValueOf(std::size_t idx) noexcept {
  if (idx < 16) return idx;
  const std::size_t octave = idx / 16;
  const std::size_t mant = idx % 16;
  const int msb = static_cast<int>(octave) + 3;
  const int shift = msb - 4;
  return (16ULL + mant) << shift;
}

}  // namespace

void StatsAggregator::RecordConnectLatency(std::uint64_t micros) noexcept {
  std::size_t idx = BucketOf(micros);
  if (idx >= kBuckets) idx = kBuckets - 1;  // clamp an implausibly large tail
  buckets_[idx].fetch_add(1, std::memory_order_relaxed);
  count_.fetch_add(1, std::memory_order_relaxed);

  std::uint64_t prev = max_micros_.load(std::memory_order_relaxed);
  while (micros > prev &&
         !max_micros_.compare_exchange_weak(prev, micros, std::memory_order_relaxed)) {
  }
}

std::uint64_t StatsAggregator::MaxConnectLatencyMicros() const noexcept {
  return max_micros_.load(std::memory_order_relaxed);
}

PerCoreCounters StatsAggregator::Aggregate(
    const std::vector<const PerCoreCounters*>& per_core) const {
  PerCoreCounters total;
  for (const auto* c : per_core) {
    if (c == nullptr) continue;
    total.accepted_connections += c->accepted_connections;
    total.active_connections += c->active_connections;
    total.bytes_client_to_upstream += c->bytes_client_to_upstream;
    total.bytes_upstream_to_client += c->bytes_upstream_to_client;
    total.connect_failures += c->connect_failures;
  }
  return total;
}

LatencyPercentiles StatsAggregator::ConnectLatencyPercentiles() const {
  LatencyPercentiles p;
  const std::uint64_t total = count_.load(std::memory_order_relaxed);
  if (total == 0) return p;

  struct Target {
    double q;
    double* out;
  };
  const Target targets[] = {
      {0.50, &p.p50}, {0.95, &p.p95}, {0.99, &p.p99}, {0.999, &p.p999}};

  std::size_t ti = 0;
  std::uint64_t cumulative = 0;
  for (std::size_t i = 0; i < kBuckets && ti < 4; ++i) {
    cumulative += buckets_[i].load(std::memory_order_relaxed);
    // A rank at or below the running total falls in this bucket.
    while (ti < 4 &&
           static_cast<double>(cumulative) >= targets[ti].q * static_cast<double>(total)) {
      *targets[ti].out = static_cast<double>(ValueOf(i));
      ++ti;
    }
  }
  return p;
}

}  // namespace sluiced
