#pragma once
// One event loop per core: owns a listening socket, an epoll instance, and
// its connection table exclusively. docs/SPEC.md §5.2 (DP-U-04..07, DP-O-01).
//
// Contract:
//  - No connection state is readable or writable by another EventLoop
//    (CLAUDE.md invariant: shared-nothing). Cross-core communication only
//    happens through the Maglev-table atomic swap (via the shared Router) and
//    relaxed stat counters — never by one loop reaching into another's table.
//  - No locks on the per-connection forwarding path (DP-U-07).
//  - The listening socket is created with SO_REUSEPORT so N loops can each
//    own an independent accept queue on the same port (DP-U-05).
//  - Where CPU pinning is enabled (core_id >= 0), Run() binds its thread to
//    that core (DP-O-01).

#include <chrono>
#include <cstdint>
#include <memory>
#include <unordered_map>

#include "sluiced/backend.hpp"
#include "sluiced/connection.hpp"
#include "sluiced/router.hpp"
#include "sluiced/stats.hpp"

namespace sluiced {

class EventLoop {
public:
    // router and stats are shared across all loops and outlive them. core_id
    // < 0 disables CPU pinning for this loop.
    EventLoop(int core_id, std::uint16_t listen_port, Router& router, StatsAggregator& stats);
    ~EventLoop();

    EventLoop(const EventLoop&) = delete;
    EventLoop& operator=(const EventLoop&) = delete;

    // Blocks, running epoll_wait until Stop() is called.
    void Run();

    // Thread-safe: only ever used to unblock Run() from another thread.
    void Stop() noexcept;

    // Read by the admin socket's stats aggregation (DP-U-15/16). Only this
    // loop's own thread writes them; reads are an approximate snapshot.
    [[nodiscard]] const PerCoreCounters& Counters() const noexcept { return counters_; }
    [[nodiscard]] int CoreId() const noexcept { return core_id_; }

    // False if the listener or epoll instance failed to set up (e.g. bind).
    [[nodiscard]] bool Valid() const noexcept { return listen_fd_ >= 0 && epoll_fd_ >= 0; }

private:
    // A fully proxied connection plus the backend it was pinned to at accept
    // time — held so the backend's in-flight counter can be decremented on
    // close, and so the Backend object outlives config swaps (DP-U-10).
    struct Established {
        std::unique_ptr<Connection> conn;
        std::shared_ptr<Backend> backend;
    };

    // An upstream socket mid-connect (non-blocking connect() returned
    // EINPROGRESS); promoted to Established when it becomes writable.
    struct Pending {
        int client_fd;
        int upstream_fd;
        std::shared_ptr<Backend> backend;
        std::chrono::steady_clock::time_point start;
    };

    void AcceptReady() noexcept;
    void StartConnect(int client_fd, std::shared_ptr<Backend> backend) noexcept;
    void Establish(int client_fd, int upstream_fd, const std::shared_ptr<Backend>& backend,
                   std::chrono::steady_clock::time_point start) noexcept;
    void ConnectFailed(int client_fd, int upstream_fd, const std::shared_ptr<Backend>& backend) noexcept;
    void FinishConnection(int client_fd, int upstream_fd, const std::shared_ptr<Backend>& backend) noexcept;

    int core_id_;
    std::uint16_t listen_port_;
    Router& router_;
    StatsAggregator& stats_;

    int epoll_fd_ = -1;
    int listen_fd_ = -1;
    int wake_fd_ = -1;  // eventfd; Stop() writes it to break epoll_wait
    bool stop_ = false;

    PerCoreCounters counters_;

    // Both fds of an established connection key the same entry, so an event on
    // either finds it. Erased (both keys) exactly once, on close.
    std::unordered_map<int, std::shared_ptr<Established>> established_;
    std::unordered_map<int, Pending> pending_;  // upstream_fd -> pending connect
};

}  // namespace sluiced
