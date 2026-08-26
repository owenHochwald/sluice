// StatsAggregator: counter summation and connect-latency percentiles.
#include <catch2/catch_test_macros.hpp>

#include <vector>

#include "sluiced/stats.hpp"

using sluiced::LatencyPercentiles;
using sluiced::PerCoreCounters;
using sluiced::StatsAggregator;

TEST_CASE("Aggregate sums per-core counters", "[stats]") {
    PerCoreCounters a, b;
    a.accepted_connections = 10;
    a.bytes_client_to_upstream = 100;
    b.accepted_connections = 5;
    b.bytes_client_to_upstream = 50;

    StatsAggregator agg;
    PerCoreCounters total = agg.Aggregate({&a, &b});
    REQUIRE(total.accepted_connections == 15);
    REQUIRE(total.bytes_client_to_upstream == 150);
}

TEST_CASE("Percentiles are zero with no samples", "[stats]") {
    StatsAggregator agg;
    LatencyPercentiles p = agg.ConnectLatencyPercentiles();
    REQUIRE(p.p50 == 0);
    REQUIRE(p.p99 == 0);
}

TEST_CASE("Percentiles are ordered and near the recorded value", "[stats]") {
    StatsAggregator agg;
    // A tight cluster around 100us; every percentile should land in that
    // neighborhood, within the histogram's ~6% bucket width.
    for (int i = 0; i < 10000; ++i) agg.RecordConnectLatency(100);
    LatencyPercentiles p = agg.ConnectLatencyPercentiles();
    REQUIRE(p.p50 >= 90);
    REQUIRE(p.p50 <= 110);
    REQUIRE(p.p99 >= 90);
    REQUIRE(p.p99 <= 110);
}

TEST_CASE("Percentiles are monotonic across a wide spread", "[stats]") {
    StatsAggregator agg;
    for (int i = 1; i <= 100000; ++i) agg.RecordConnectLatency(static_cast<std::uint64_t>(i));
    LatencyPercentiles p = agg.ConnectLatencyPercentiles();
    REQUIRE(p.p50 <= p.p95);
    REQUIRE(p.p95 <= p.p99);
    REQUIRE(p.p99 <= p.p999);
    REQUIRE(agg.MaxConnectLatencyMicros() == 100000);
    // p50 of a uniform 1..100000 spread should sit near the middle.
    REQUIRE(p.p50 >= 40000);
    REQUIRE(p.p50 <= 60000);
}
