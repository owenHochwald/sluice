#include "sluiced/health_checker.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>

namespace sluiced {

namespace {

bool ParseEndpoint(const std::string& addr, sockaddr_storage& ss, socklen_t& len) noexcept {
    const auto pos = addr.rfind(':');
    if (pos == std::string::npos) return false;
    std::string host = addr.substr(0, pos);
    const int port = std::atoi(addr.c_str() + pos + 1);
    if (host.size() >= 2 && host.front() == '[' && host.back() == ']') {
        host = host.substr(1, host.size() - 2);
    }
    std::memset(&ss, 0, sizeof(ss));
    auto* v4 = reinterpret_cast<sockaddr_in*>(&ss);
    if (::inet_pton(AF_INET, host.c_str(), &v4->sin_addr) == 1) {
        v4->sin_family = AF_INET;
        v4->sin_port = htons(static_cast<std::uint16_t>(port));
        len = sizeof(sockaddr_in);
        return true;
    }
    auto* v6 = reinterpret_cast<sockaddr_in6*>(&ss);
    if (::inet_pton(AF_INET6, host.c_str(), &v6->sin6_addr) == 1) {
        v6->sin6_family = AF_INET6;
        v6->sin6_port = htons(static_cast<std::uint16_t>(port));
        len = sizeof(sockaddr_in6);
        return true;
    }
    return false;
}

}  // namespace

HealthChecker::HealthChecker(HealthCheckerConfig config) : config_(config) {}

bool HealthChecker::ActiveProbe(const std::string& address) const noexcept {
    // DP-U-11: establish and immediately close a TCP connection. Non-blocking
    // connect + poll so a hung backend can't stall the whole check round.
    sockaddr_storage ss;
    socklen_t sl = 0;
    if (!ParseEndpoint(address, ss, sl)) return false;

    const int fd = ::socket(ss.ss_family, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) return false;

    bool ok = false;
    const int r = ::connect(fd, reinterpret_cast<sockaddr*>(&ss), sl);
    if (r == 0) {
        ok = true;
    } else if (errno == EINPROGRESS) {
        pollfd pfd{fd, POLLOUT, 0};
        if (::poll(&pfd, 1, static_cast<int>(config_.connect_timeout_ms)) > 0) {
            int err = 0;
            socklen_t el = sizeof(err);
            ::getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &el);
            ok = (err == 0);
        }
    }
    ::close(fd);
    return ok;
}

bool HealthChecker::CheckOnce(const BackendList& backends) {
    const auto now = std::chrono::steady_clock::now();
    bool changed = false;

    for (const auto& b : backends) {
        State& s = state_[b->address];

        // --- active health check (DP-E-05/06) ---
        if (ActiveProbe(b->address)) {
            s.consecutive_passes++;
            s.consecutive_failures = 0;
            if (b->health_state.load(std::memory_order_relaxed) == HealthState::kUnhealthy &&
                s.consecutive_passes >= config_.healthy_threshold) {
                b->health_state.store(HealthState::kHealthy, std::memory_order_relaxed);
                changed = true;
            }
        } else {
            s.consecutive_failures++;
            s.consecutive_passes = 0;
            if (b->health_state.load(std::memory_order_relaxed) == HealthState::kHealthy &&
                s.consecutive_failures >= config_.unhealthy_threshold) {
                b->health_state.store(HealthState::kUnhealthy, std::memory_order_relaxed);
                changed = true;
            }
        }

        // --- passive outlier ejection (DP-E-07/08) ---
        const std::uint64_t errors = b->connect_failures.load(std::memory_order_relaxed) +
                                     b->abnormal_closes.load(std::memory_order_relaxed);
        const std::uint64_t total = b->connections_total.load(std::memory_order_relaxed);
        if (!s.seeded) {
            s.last_errors = errors;
            s.last_total = total;
            s.seeded = true;
        }
        const std::uint64_t d_errors = errors - s.last_errors;
        const std::uint64_t d_total = total - s.last_total;
        s.last_errors = errors;
        s.last_total = total;

        // A "sample" is any connect attempt: successes established (d_total)
        // plus outright connect failures (part of d_errors).
        const std::uint64_t attempts = d_total + d_errors;
        const bool currently_ejected = b->ejected.load(std::memory_order_relaxed);

        if (currently_ejected) {
            if (now >= s.eject_until) {
                b->ejected.store(false, std::memory_order_relaxed);
                changed = true;  // give it a chance again; re-eval next round
            }
        } else if (attempts >= config_.min_passive_samples) {
            const double rate = static_cast<double>(d_errors) / static_cast<double>(attempts);
            if (rate > config_.error_rate_threshold) {
                s.ejection_count++;
                // DP-E-08: multiplicative backoff, capped.
                std::uint64_t interval = static_cast<std::uint64_t>(config_.base_ejection_ms)
                                         << (s.ejection_count - 1);
                if (interval > config_.max_ejection_ms) interval = config_.max_ejection_ms;
                s.ejection_ms = static_cast<std::uint32_t>(interval);
                s.eject_until = now + std::chrono::milliseconds(s.ejection_ms);
                b->ejected.store(true, std::memory_order_relaxed);
                changed = true;
            }
        }
    }

    return changed;
}

bool HealthChecker::ShouldPanic(std::size_t ejected_count, std::size_t total_count) const noexcept {
    if (total_count == 0) return false;
    return static_cast<double>(ejected_count) > config_.panic_threshold * static_cast<double>(total_count);
}

}  // namespace sluiced
