#pragma once
// Backend bookkeeping: identity plus the health/ejection/counter state the
// data plane maintains about it. docs/SPEC.md §3 "Backend", DP-U-12.
//
// TODO(owen): the struct below is a reasonable starting shape but you own
// it — extend as health_checker.hpp and stats.hpp need. Whatever you add,
// keep it atomics updated from the forwarding path and read elsewhere,
// never a lock (DP-U-13: relaxed atomics, aggregated on a timer).

#include <atomic>
#include <cstdint>
#include <string>

namespace sluiced {

enum class HealthState { kHealthy, kUnhealthy };

struct Backend {
    std::string address; // "ip:port"

    // Active health check state (DP-U-11, DP-E-05/06).
    std::atomic<HealthState> health_state{HealthState::kHealthy};

    // Passive ejection state (DP-U-12, DP-E-07/08).
    std::atomic<bool> ejected{false};
    std::atomic<std::uint64_t> connect_failures{0};
    std::atomic<std::uint64_t> abnormal_closes{0};

    // Operator drain: excluded from new-connection selection while existing
    // connections finish (CLI-E-01/02).
    std::atomic<bool> drained{false};

    // Read by admin_socket.hpp, and by power-of-two-choices selection if
    // enabled (DP-O-02).
    std::atomic<std::uint64_t> active_connections{0};

    // Cumulative connections successfully established to this backend — the
    // denominator the passive error-rate window divides against (DP-U-12).
    std::atomic<std::uint64_t> connections_total{0};
};

}  // namespace sluiced
