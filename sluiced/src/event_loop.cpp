#include "sluiced/event_loop.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <sched.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>

namespace sluiced {

namespace {

// FNV-1a over the client endpoint bytes. The proxy's listen address is fixed,
// so the client's ip:port carries all the 5-tuple entropy the Maglev lookup
// needs (DP-E-04).
std::uint64_t HashEndpoint(const sockaddr_storage& ss, socklen_t len) noexcept {
    std::uint64_t h = 14695981039346656037ULL;
    const auto* p = reinterpret_cast<const unsigned char*>(&ss);
    for (socklen_t i = 0; i < len; ++i) {
        h ^= p[i];
        h *= 1099511628211ULL;
    }
    return h;
}

// Parse "ip:port" (numeric, v4 or v6) into a sockaddr. No DNS, no allocation
// on the resolver path.
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

void SetNoDelay(int fd) noexcept {
    int one = 1;
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
}

// A listening socket per loop, all sharing the port via SO_REUSEPORT so the
// kernel spreads accepts across the loops (DP-U-05).
int MakeListener(std::uint16_t port) noexcept {
    int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    int one = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof(one));
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    a.sin_port = htons(port);
    if (::bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof(a)) < 0 ||
        ::listen(fd, SOMAXCONN) < 0) {
        ::close(fd);
        return -1;
    }
    return fd;
}

}  // namespace

EventLoop::EventLoop(int core_id, std::uint16_t listen_port, Router& router, StatsAggregator& stats)
    : core_id_(core_id), listen_port_(listen_port), router_(router), stats_(stats) {
    epoll_fd_ = ::epoll_create1(EPOLL_CLOEXEC);
    listen_fd_ = MakeListener(listen_port);
    wake_fd_ = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);

    epoll_event ev{};
    ev.events = EPOLLIN;
    ev.data.fd = listen_fd_;
    ::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, listen_fd_, &ev);
    ev.data.fd = wake_fd_;
    ::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, wake_fd_, &ev);
}

EventLoop::~EventLoop() {
    for (auto& [fd, p] : pending_) {
        ::close(p.client_fd);
        ::close(p.upstream_fd);
    }
    // Established connections close their own fds via Connection's destructor
    // when the map is cleared.
    established_.clear();
    if (listen_fd_ >= 0) ::close(listen_fd_);
    if (wake_fd_ >= 0) ::close(wake_fd_);
    if (epoll_fd_ >= 0) ::close(epoll_fd_);
}

void EventLoop::Run() {
    if (core_id_ >= 0) {
        cpu_set_t set;
        CPU_ZERO(&set);
        CPU_SET(static_cast<unsigned>(core_id_), &set);
        ::pthread_setaffinity_np(::pthread_self(), sizeof(set), &set);
    }

    std::array<epoll_event, 1024> events;
    while (!stop_) {
        const int n = ::epoll_wait(epoll_fd_, events.data(), events.size(), -1);
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }

        bool accept_ready = false;
        for (int i = 0; i < n; ++i) {
            const int fd = events[i].data.fd;
            const std::uint32_t flags = events[i].events;

            if (fd == listen_fd_) {
                accept_ready = true;
                continue;
            }
            if (fd == wake_fd_) {
                std::uint64_t v;
                while (::read(wake_fd_, &v, sizeof(v)) > 0) {
                }
                stop_ = true;
                continue;
            }

            if (auto pit = pending_.find(fd); pit != pending_.end()) {
                Pending p = pit->second;
                pending_.erase(pit);
                int err = 0;
                socklen_t el = sizeof(err);
                ::getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &el);
                if (err != 0 || (flags & (EPOLLERR | EPOLLHUP))) {
                    ConnectFailed(p.client_fd, p.upstream_fd, p.backend);
                } else {
                    // Drop the connect-time EPOLLOUT registration so
                    // Connection's fresh EPOLLIN add on this fd is clean.
                    ::epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, p.upstream_fd, nullptr);
                    Establish(p.client_fd, p.upstream_fd, p.backend, p.start);
                }
                continue;
            }

            if (auto it = established_.find(fd); it != established_.end()) {
                auto entry = it->second;  // keep alive across the callbacks
                bool alive = true;
                if (flags & EPOLLOUT) alive = entry->conn->OnWritable(fd);
                if (alive && (flags & (EPOLLIN | EPOLLHUP | EPOLLERR))) {
                    alive = entry->conn->OnReadable(fd);
                }
                if (!alive) {
                    FinishConnection(entry->conn->ClientFd(), entry->conn->UpstreamFd(),
                                     entry->backend);
                }
            }
        }

        // Accept only after every connection event in this batch, so a fd that
        // a connection closed this round can't be reused by an accept while a
        // stale event for it is still queued behind it.
        if (accept_ready) AcceptReady();
    }
}

void EventLoop::Stop() noexcept {
    std::uint64_t one = 1;
    if (wake_fd_ >= 0) {
        [[maybe_unused]] ssize_t r = ::write(wake_fd_, &one, sizeof(one));
    }
}

void EventLoop::AcceptReady() noexcept {
    for (;;) {
        sockaddr_storage ss;
        socklen_t sl = sizeof(ss);
        const int cfd = ::accept4(listen_fd_, reinterpret_cast<sockaddr*>(&ss), &sl,
                                  SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (cfd < 0) {
            if (errno == EINTR) continue;
            break;  // EAGAIN/EWOULDBLOCK: drained the accept queue
        }
        ++counters_.accepted_connections;

        auto backend = router_.Select(HashEndpoint(ss, sl));
        if (!backend) {
            ::close(cfd);  // no eligible backend: nothing to proxy to
            continue;
        }
        SetNoDelay(cfd);
        StartConnect(cfd, std::move(backend));
    }
}

void EventLoop::StartConnect(int client_fd, std::shared_ptr<Backend> backend) noexcept {
    sockaddr_storage ss;
    socklen_t sl = 0;
    if (!ParseEndpoint(backend->address, ss, sl)) {
        ConnectFailed(client_fd, -1, backend);
        return;
    }
    const int ufd = ::socket(ss.ss_family, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (ufd < 0) {
        ConnectFailed(client_fd, -1, backend);
        return;
    }
    SetNoDelay(ufd);

    const auto start = std::chrono::steady_clock::now();
    const int r = ::connect(ufd, reinterpret_cast<sockaddr*>(&ss), sl);
    if (r == 0) {
        Establish(client_fd, ufd, backend, start);
        return;
    }
    if (errno == EINPROGRESS) {
        epoll_event ev{};
        ev.events = EPOLLOUT;
        ev.data.fd = ufd;
        ::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, ufd, &ev);
        pending_[ufd] = Pending{client_fd, ufd, std::move(backend), start};
        return;
    }
    ConnectFailed(client_fd, ufd, backend);
}

void EventLoop::Establish(int client_fd, int upstream_fd, const std::shared_ptr<Backend>& backend,
                          std::chrono::steady_clock::time_point start) noexcept {
    const auto micros = std::chrono::duration_cast<std::chrono::microseconds>(
                            std::chrono::steady_clock::now() - start)
                            .count();
    stats_.RecordConnectLatency(static_cast<std::uint64_t>(micros));

    backend->active_connections.fetch_add(1, std::memory_order_relaxed);
    backend->connections_total.fetch_add(1, std::memory_order_relaxed);
    ++counters_.active_connections;

    auto entry = std::make_shared<Established>();
    entry->backend = backend;
    entry->conn = std::make_unique<Connection>(epoll_fd_, client_fd, upstream_fd);
    entry->conn->SetByteSinks(&counters_.bytes_client_to_upstream,
                              &counters_.bytes_upstream_to_client);
    established_[client_fd] = entry;
    established_[upstream_fd] = entry;
}

void EventLoop::ConnectFailed(int client_fd, int upstream_fd,
                              const std::shared_ptr<Backend>& backend) noexcept {
    // DP-X-01: count the failure against the backend, close the client, and do
    // not retry this connection on the same backend.
    backend->connect_failures.fetch_add(1, std::memory_order_relaxed);
    ++counters_.connect_failures;
    if (upstream_fd >= 0) {
        ::epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, upstream_fd, nullptr);
        ::close(upstream_fd);
    }
    ::close(client_fd);
}

void EventLoop::FinishConnection(int client_fd, int upstream_fd,
                                 const std::shared_ptr<Backend>& backend) noexcept {
    backend->active_connections.fetch_sub(1, std::memory_order_relaxed);
    if (counters_.active_connections > 0) --counters_.active_connections;
    established_.erase(client_fd);
    established_.erase(upstream_fd);
}

}  // namespace sluiced
