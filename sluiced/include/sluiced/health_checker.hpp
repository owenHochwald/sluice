#pragma once
// Active health checking and passive outlier ejection.
// docs/SPEC.md §5.4 (DP-U-11..13, DP-E-05..08, DP-X-03).
//
// Contract:
//  - Active: connect-and-immediately-close every backend on a configurable
//    interval; K consecutive failures -> unhealthy + excluded from the
//    Maglev table, M consecutive passes -> healthy + reincluded (DP-E-05/06).
//  - Passive: a sliding-window error rate (connect failures + abnormal
//    closes) per backend; crossing a threshold ejects it for a base
//    interval that grows multiplicatively on repeat offenses, capped at a
//    configurable maximum (DP-E-07/08).
//  - Panic threshold — DP-X-03, the one that matters most: if ejecting
//    would remove more than a configurable fraction (default 50%) of
//    backends, disregard all ejections and serve the full healthy set
//    instead. Half the fleet failing is far more likely to be a broken
//    detector or a local network blip than an actual outage; don't let a
//    false positive convert into a total one.
//  - Ejection state is shared across event loops via relaxed atomics
//    aggregated on a timer (DP-U-13) — see Backend's atomics in
//    backend.hpp. No synchronization on the forwarding path.
//
// TODO(owen): implement in src/health_checker.cpp. DP-X-03 is a real design
// decision, not boilerplate — read the rationale in docs/SPEC.md §5.4
// before writing it.

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "sluiced/backend.hpp"

namespace sluiced {

struct HealthCheckerConfig {
    std::uint32_t interval_ms = 5000;
    std::uint32_t unhealthy_threshold = 3; // K consecutive failures
    std::uint32_t healthy_threshold = 2;   // M consecutive passes
    double error_rate_threshold = 0.5;     // triggers passive ejection
    std::uint32_t base_ejection_ms = 30000;
    std::uint32_t max_ejection_ms = 300000;
    double panic_threshold = 0.5; // DP-X-03: fraction of backends
    std::uint32_t connect_timeout_ms = 1000; // active-check TCP connect timeout
    std::uint64_t min_passive_samples = 20;  // don't eject on a tiny sample
};

// Backends are held as shared_ptr (not a contiguous span) because Backend
// carries atomics and must keep a stable address across config swaps while a
// connection still references it — see event_loop.hpp / router.hpp.
using BackendList = std::vector<std::shared_ptr<Backend>>;

class HealthChecker {
public:
    explicit HealthChecker(HealthCheckerConfig config);

    // One round of active health checks plus a passive error-rate evaluation
    // against every backend, mutating each backend's health_state and ejected
    // flags. Meant to be called on a timer, off the forwarding path. Returns
    // true if any backend's health or ejection state changed (i.e. the caller
    // should rebuild and republish the Maglev table).
    bool CheckOnce(const BackendList& backends);

    // Applies DP-X-03: given how many backends ejection would currently
    // remove, returns true if ejections should be disregarded this round.
    [[nodiscard]] bool ShouldPanic(std::size_t ejected_count, std::size_t total_count) const noexcept;

private:
    // Per-backend detector state, keyed by address so it survives the Backend
    // object being recreated on a membership change.
    struct State {
        std::uint32_t consecutive_failures = 0;
        std::uint32_t consecutive_passes = 0;
        std::uint64_t last_errors = 0;   // connect_failures + abnormal_closes
        std::uint64_t last_total = 0;    // connections_total
        std::uint32_t ejection_ms = 0;   // current backoff interval
        std::uint32_t ejection_count = 0;
        std::chrono::steady_clock::time_point eject_until{};
        bool seeded = false;
    };

    // A single active-check TCP connect-and-close; true on success.
    bool ActiveProbe(const std::string& address) const noexcept;

    HealthCheckerConfig config_;
    std::unordered_map<std::string, State> state_;
};

}  // namespace sluiced
