#include "sluiced/admin_socket.hpp"

#include <poll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <sstream>

namespace sluiced {

namespace {

std::string Quote(const std::string& s) {
    std::string out = "\"";
    for (char c : s) {
        if (c == '"' || c == '\\') out += '\\';
        out += c;
    }
    out += '"';
    return out;
}

const char* HealthName(const Backend& b) noexcept {
    return b.health_state.load(std::memory_order_relaxed) == HealthState::kHealthy ? "healthy"
                                                                                   : "unhealthy";
}

}  // namespace

AdminSocket::AdminSocket(std::string socket_path, Router& router, StatsAggregator& stats,
                         std::vector<const PerCoreCounters*> per_core, std::string config_source)
    : socket_path_(std::move(socket_path)),
      router_(router),
      stats_(stats),
      per_core_(std::move(per_core)),
      config_source_(std::move(config_source)),
      started_(std::chrono::steady_clock::now()) {}

AdminSocket::~AdminSocket() {
    if (listen_fd_ >= 0) ::close(listen_fd_);
    if (wake_fd_ >= 0) ::close(wake_fd_);
    ::unlink(socket_path_.c_str());
}

void AdminSocket::Run() {
    listen_fd_ = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (listen_fd_ < 0) return;
    wake_fd_ = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, socket_path_.c_str(), sizeof(addr.sun_path) - 1);
    ::unlink(socket_path_.c_str());
    if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0 ||
        ::listen(listen_fd_, 16) < 0) {
        std::fprintf(stderr, "admin: cannot bind %s: %s\n", socket_path_.c_str(),
                     std::strerror(errno));
        return;
    }

    running_.store(true);
    while (running_.load()) {
        pollfd pfds[2] = {{listen_fd_, POLLIN, 0}, {wake_fd_, POLLIN, 0}};
        const int r = ::poll(pfds, 2, -1);
        if (r < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (pfds[1].revents & POLLIN) break;  // Stop() poked the eventfd
        if (!(pfds[0].revents & POLLIN)) continue;

        const int cfd = ::accept(listen_fd_, nullptr, nullptr);
        if (cfd < 0) continue;

        std::string cmd;
        char buf[512];
        for (;;) {
            const ssize_t n = ::read(cfd, buf, sizeof(buf));
            if (n <= 0) break;
            cmd.append(buf, static_cast<std::size_t>(n));
            if (cmd.find('\n') != std::string::npos) break;
        }
        const auto nl = cmd.find('\n');
        if (nl != std::string::npos) cmd.resize(nl);

        const std::string reply = Handle(cmd);
        ::write(cfd, reply.data(), reply.size());
        ::close(cfd);
    }
}

void AdminSocket::Stop() noexcept {
    running_.store(false);
    std::uint64_t one = 1;
    if (wake_fd_ >= 0) {
        [[maybe_unused]] ssize_t r = ::write(wake_fd_, &one, sizeof(one));
    }
}

std::string AdminSocket::Handle(const std::string& command) {
    std::istringstream in(command);
    std::string verb, arg;
    in >> verb >> arg;

    std::ostringstream out;

    if (verb == "backends") {
        auto state = router_.Snapshot();
        out << "{\"backends\":[";
        if (state) {
            for (std::size_t i = 0; i < state->backends.size(); ++i) {
                const Backend& b = *state->backends[i];
                if (i) out << ",";
                out << "{" << "\"address\":" << Quote(b.address)
                    << ",\"health\":" << Quote(HealthName(b))
                    << ",\"ejected\":" << (b.ejected.load(std::memory_order_relaxed) ? "true" : "false")
                    << ",\"drained\":" << (b.drained.load(std::memory_order_relaxed) ? "true" : "false")
                    << ",\"active_connections\":" << b.active_connections.load(std::memory_order_relaxed)
                    << ",\"connect_failures\":" << b.connect_failures.load(std::memory_order_relaxed)
                    << ",\"connections_total\":" << b.connections_total.load(std::memory_order_relaxed)
                    << "}";
            }
        }
        out << "]}";
    } else if (verb == "stats") {
        PerCoreCounters agg = stats_.Aggregate(per_core_);
        LatencyPercentiles p = stats_.ConnectLatencyPercentiles();
        out << "{\"aggregate\":{"
            << "\"accepted\":" << agg.accepted_connections
            << ",\"active\":" << agg.active_connections
            << ",\"bytes_c2u\":" << agg.bytes_client_to_upstream
            << ",\"bytes_u2c\":" << agg.bytes_upstream_to_client
            << ",\"connect_failures\":" << agg.connect_failures << "}"
            << ",\"connect_latency_us\":{"
            << "\"p50\":" << p.p50 << ",\"p95\":" << p.p95 << ",\"p99\":" << p.p99
            << ",\"p999\":" << p.p999 << ",\"max\":" << stats_.MaxConnectLatencyMicros() << "}"
            << ",\"per_core\":[";
        for (std::size_t i = 0; i < per_core_.size(); ++i) {
            const auto* c = per_core_[i];
            if (i) out << ",";
            out << "{\"accepted\":" << c->accepted_connections
                << ",\"active\":" << c->active_connections
                << ",\"bytes_c2u\":" << c->bytes_client_to_upstream
                << ",\"bytes_u2c\":" << c->bytes_upstream_to_client
                << ",\"connect_failures\":" << c->connect_failures << "}";
        }
        out << "]}";
    } else if (verb == "config") {
        const auto age = std::chrono::duration_cast<std::chrono::seconds>(
                             std::chrono::steady_clock::now() - started_)
                             .count();
        out << "{\"version\":" << router_.Version()
            << ",\"source\":" << Quote(config_source_)
            << ",\"age_seconds\":" << age << "}";
    } else if (verb == "drain") {
        const bool ok = router_.Drain(arg);
        out << "{\"drained\":" << Quote(arg) << ",\"ok\":" << (ok ? "true" : "false") << "}";
    } else if (verb == "undrain") {
        const bool ok = router_.Undrain(arg);
        out << "{\"undrained\":" << Quote(arg) << ",\"ok\":" << (ok ? "true" : "false") << "}";
    } else {
        out << "{\"error\":\"unknown command\"}";
    }

    out << "\n";
    return out.str();
}

}  // namespace sluiced
