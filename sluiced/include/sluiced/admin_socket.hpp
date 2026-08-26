#pragma once
// Unix-domain admin socket: the transport sluicectl talks over.
// docs/SPEC.md DP-U-14, §7 (CLI-*).
//
// Contract:
//  - One Unix-domain listening socket, off the forwarding path.
//  - Commands to support, matching sluicectl's subcommands:
//      backends          -> health state, ejection state, active conns,
//                            error-window rate, per backend (CLI-U-02)
//      stats             -> aggregated + per-core counters, throughput,
//                            latency percentiles (CLI-U-03)
//      config            -> current config version, source, age (CLI-U-04)
//      drain <backend>   -> stop selecting for new conns, let existing
//                            finish (CLI-E-01)
//      undrain <backend> -> resume selecting (CLI-E-02)
//  - Wire format is intentionally unspecified here — pick something simple
//    (line-based text or small JSON) sluicectl can parse without a shared
//    schema. This is not the gRPC config stream; it's a low-stakes local
//    control surface.
//
// Implemented in src/admin_socket.cpp. Responses are JSON (one object per
// command), which sluicectl can parse without a shared schema and which is
// trivial to eyeball with socat/nc.

#include <atomic>
#include <chrono>
#include <string>
#include <vector>

#include "sluiced/router.hpp"
#include "sluiced/stats.hpp"

namespace sluiced {

class AdminSocket {
public:
    // router, stats, and per_core all outlive the AdminSocket. per_core points
    // at each EventLoop's own counters; the socket only reads them (DP-U-16).
    AdminSocket(std::string socket_path, Router& router, StatsAggregator& stats,
                std::vector<const PerCoreCounters*> per_core, std::string config_source);
    ~AdminSocket();

    AdminSocket(const AdminSocket&) = delete;
    AdminSocket& operator=(const AdminSocket&) = delete;

    // Blocks, accepting and handling one command per connection.
    void Run();
    void Stop() noexcept;

private:
    std::string Handle(const std::string& command);

    std::string socket_path_;
    Router& router_;
    StatsAggregator& stats_;
    std::vector<const PerCoreCounters*> per_core_;
    std::string config_source_;
    std::chrono::steady_clock::time_point started_;

    int listen_fd_ = -1;
    int wake_fd_ = -1;
    std::atomic<bool> running_{false};
};

}  // namespace sluiced
