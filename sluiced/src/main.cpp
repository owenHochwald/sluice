// sluiced entrypoint: parse config, load the bootstrap backend set, spin up N
// shared-nothing event loops behind SO_REUSEPORT, run the health checker and
// admin socket on their own threads, and (when built with gRPC) watch the
// controller for config updates. docs/SPEC.md §5.

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "sluiced/admin_socket.hpp"
#include "sluiced/config_stream.hpp"
#include "sluiced/event_loop.hpp"
#include "sluiced/health_checker.hpp"
#include "sluiced/router.hpp"
#include "sluiced/stats.hpp"

namespace {

std::sig_atomic_t volatile g_stop = 0;
void OnSignal(int) { g_stop = 1; }

struct Options {
    std::uint16_t listen_port = 8080;
    int workers = 0;  // 0 => hardware_concurrency
    std::string bootstrap = "backends.conf";
    std::string controller;  // empty => bootstrap-only, fail-static
    std::string admin_socket = "/tmp/sluiced.sock";
    bool pin = false;
    bool pow2 = false;
    std::uint32_t health_interval_ms = 2000;
    std::size_t table_size = 65537;
};

const char* ArgValue(int argc, char** argv, int& i) {
    if (i + 1 >= argc) {
        std::fprintf(stderr, "missing value for %s\n", argv[i]);
        std::exit(2);
    }
    return argv[++i];
}

Options ParseArgs(int argc, char** argv) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--listen-port") o.listen_port = static_cast<std::uint16_t>(std::atoi(ArgValue(argc, argv, i)));
        else if (a == "--workers") o.workers = std::atoi(ArgValue(argc, argv, i));
        else if (a == "--bootstrap") o.bootstrap = ArgValue(argc, argv, i);
        else if (a == "--controller") o.controller = ArgValue(argc, argv, i);
        else if (a == "--admin-socket") o.admin_socket = ArgValue(argc, argv, i);
        else if (a == "--pin") o.pin = true;
        else if (a == "--pow2") o.pow2 = true;
        else if (a == "--health-interval-ms") o.health_interval_ms = static_cast<std::uint32_t>(std::atoi(ArgValue(argc, argv, i)));
        else if (a == "--table-size") o.table_size = static_cast<std::size_t>(std::atoll(ArgValue(argc, argv, i)));
        else {
            std::fprintf(stderr, "unknown flag %s\n", a.c_str());
            std::exit(2);
        }
    }
    return o;
}

}  // namespace

int main(int argc, char** argv) {
    const Options opt = ParseArgs(argc, argv);

    int workers = opt.workers > 0 ? opt.workers
                                  : static_cast<int>(std::thread::hardware_concurrency());
    if (workers < 1) workers = 1;

    sluiced::Router router(opt.table_size, opt.pow2);
    sluiced::StatsAggregator stats;

    // Load the bootstrap set before any loop starts serving (DP-E-09).
    sluiced::ConfigStreamClient config(
        opt.bootstrap, opt.controller,
        [&router](std::uint64_t version, std::vector<std::string> addresses) {
            router.SetBackends(version, addresses);
        });
    config.LoadBootstrap();

    // Event loops: one per worker, each with its own SO_REUSEPORT listener.
    std::vector<std::unique_ptr<sluiced::EventLoop>> loops;
    std::vector<const sluiced::PerCoreCounters*> counters;
    for (int i = 0; i < workers; ++i) {
        const int core = opt.pin ? i : -1;
        auto loop = std::make_unique<sluiced::EventLoop>(core, opt.listen_port, router, stats);
        if (!loop->Valid()) {
            std::fprintf(stderr, "fatal: event loop %d failed to bind port %u\n", i, opt.listen_port);
            return 1;
        }
        counters.push_back(&loop->Counters());
        loops.push_back(std::move(loop));
    }

    std::vector<std::thread> loop_threads;
    for (auto& loop : loops) loop_threads.emplace_back([&loop] { loop->Run(); });

    // Health checker on its own timer thread (off the forwarding path).
    sluiced::HealthCheckerConfig hc_cfg;
    hc_cfg.interval_ms = opt.health_interval_ms;
    sluiced::HealthChecker checker(hc_cfg);
    std::atomic<bool> health_running{true};
    std::thread health_thread([&] {
        while (health_running.load()) {
            if (checker.CheckOnce(router.Backends())) {
                router.Republish(hc_cfg.panic_threshold);
            }
            for (std::uint32_t slept = 0; slept < opt.health_interval_ms && health_running.load();
                 slept += 50) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
        }
    });

    // Admin socket.
    sluiced::AdminSocket admin(opt.admin_socket, router, stats, counters,
                               opt.controller.empty() ? "bootstrap" : "controller");
    std::thread admin_thread([&admin] { admin.Run(); });

    config.Start();

    std::signal(SIGINT, OnSignal);
    std::signal(SIGTERM, OnSignal);
    std::fprintf(stderr, "sluiced: listening on :%u, %d workers, admin %s\n", opt.listen_port,
                 workers, opt.admin_socket.c_str());

    while (!g_stop) std::this_thread::sleep_for(std::chrono::milliseconds(100));

    std::fprintf(stderr, "sluiced: shutting down\n");
    config.Stop();
    health_running.store(false);
    if (health_thread.joinable()) health_thread.join();
    admin.Stop();
    if (admin_thread.joinable()) admin_thread.join();
    for (auto& loop : loops) loop->Stop();
    for (auto& t : loop_threads) t.join();
    return 0;
}
