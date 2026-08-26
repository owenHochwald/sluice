#include "sluiced/connection.hpp"

#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>

namespace sluiced {

Connection::Connection(int epoll_fd, int client_fd, int upstream_fd) noexcept
    : epoll_fd_(epoll_fd), client_fd_(client_fd), upstream_fd_(upstream_fd) {
    Register(client_fd_);
    Register(upstream_fd_);
}

Connection::~Connection() {
    // MaybeFinish() is the normal path and already closed both fds. This is
    // just the backstop for a Connection torn down early — closed_ ensures
    // only one of the two paths ever actually calls close() (DP-U-03).
    if (closed_) return;
    ::epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, client_fd_, nullptr);
    ::epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, upstream_fd_, nullptr);
    ::close(client_fd_);
    ::close(upstream_fd_);
}

// --- epoll interest ---------------------------------------------------
//
// Each fd is a *source* for one direction (wants EPOLLIN) and a *sink* for
// the other (wants EPOLLOUT, but only while a write to it is blocked) —
// both can be live at once, so interest is a per-fd bitmask, changed one
// bit at a time. EPOLLOUT is never left armed once unblocked: a socket is
// writable almost always, so watching it permanently would spin epoll_wait
// forever for no reason.

void Connection::Register(int fd) noexcept {
    epoll_event ev{};
    ev.events = EPOLLIN;
    ev.data.fd = fd;
    ::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, fd, &ev);
    (fd == client_fd_ ? client_interest_ : upstream_interest_) = EPOLLIN;
}

void Connection::SetInterestBit(int fd, std::uint32_t bit, bool set) noexcept {
    std::uint32_t& interest = (fd == client_fd_) ? client_interest_ : upstream_interest_;
    std::uint32_t updated = set ? (interest | bit) : (interest & ~bit);
    if (updated == interest) return;  // no change, skip the syscall
    interest = updated;
    epoll_event ev{};
    ev.events = interest;
    ev.data.fd = fd;
    ::epoll_ctl(epoll_fd_, EPOLL_CTL_MOD, fd, &ev);
}

void Connection::ArmRead(int fd) noexcept { SetInterestBit(fd, EPOLLIN, true); }
void Connection::StopReading(int fd) noexcept { SetInterestBit(fd, EPOLLIN, false); }
void Connection::ArmWrite(int fd) noexcept { SetInterestBit(fd, EPOLLOUT, true); }
void Connection::DisarmWrite(int fd) noexcept { SetInterestBit(fd, EPOLLOUT, false); }

// --- forwarding ---------------------------------------------------------
//
// Invariant: we only ever read a source when its buffer is fully drained
// (offset == len), and TryFlush is the only thing that re-arms reading. So
// backpressure (DP-S-01/02) is just "don't re-arm read until flushed" —
// there's no separate check for it anywhere.

void Connection::TryRead(int source_fd, int sink_fd, Direction& dir) noexcept {
    ssize_t n = ::read(source_fd, dir.buf.data(), dir.buf.size());

    if (n > 0) {
        dir.len = static_cast<std::size_t>(n);
        dir.offset = 0;
        TryFlush(source_fd, sink_fd, dir);  // try to forward it now, not on the next event
        return;
    }
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
        return;  // spurious wakeup, stay armed
    }
    // n == 0 (orderly EOF) or a real read error: either way, nothing more
    // will ever come from source_fd. That's DP-E-02's trigger.
    dir.source_eof = true;
    StopReading(source_fd);
    MaybeHalfClose(sink_fd, dir);
}

void Connection::TryFlush(int source_fd, int sink_fd, Direction& dir) noexcept {
    while (dir.offset < dir.len) {
        ssize_t n = ::write(sink_fd, dir.buf.data() + dir.offset, dir.len - dir.offset);
        if (n >= 0) {
            dir.offset += static_cast<std::size_t>(n);
            continue;
        }
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            // DP-X-02: dir.offset is already the resume position. DP-S-01/02:
            // stop reading source_fd until sink_fd drains.
            ArmWrite(sink_fd);
            StopReading(source_fd);
            return;
        }
        // Real write error (e.g. ECONNRESET): nothing left to do with the
        // unwritten remainder, so drop it and call this direction done.
        dir.offset = dir.len;
        dir.source_eof = true;
        StopReading(source_fd);
        break;
    }
    // Only reached fully drained (the EAGAIN case above returns early).
    DisarmWrite(sink_fd);
    if (!dir.source_eof) ArmRead(source_fd);
    MaybeHalfClose(sink_fd, dir);
}

void Connection::MaybeHalfClose(int sink_fd, Direction& dir) noexcept {
    if (dir.source_eof && dir.offset == dir.len && !dir.sink_shutdown) {
        // Source is done and everything it sent is out the door — tell the
        // other side. Must wait for the flush, or this would cut off bytes
        // still queued to go out.
        ::shutdown(sink_fd, SHUT_WR);
        dir.sink_shutdown = true;
    }
    MaybeFinish();
}

void Connection::MaybeFinish() noexcept {
    if (closed_) return;
    if (!c2u_.sink_shutdown || !u2c_.sink_shutdown) return;
    // Both directions forwarded everything and told their peer so — the
    // one and only close() call site (DP-U-03).
    ::epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, client_fd_, nullptr);
    ::epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, upstream_fd_, nullptr);
    ::close(client_fd_);
    ::close(upstream_fd_);
    closed_ = true;
}

// --- entry points from EventLoop ----------------------------------------

bool Connection::OnReadable(int fd) noexcept {
    if (closed_) return false;
    if (fd == client_fd_) TryRead(client_fd_, upstream_fd_, c2u_);
    else if (fd == upstream_fd_) TryRead(upstream_fd_, client_fd_, u2c_);
    return !closed_;
}

bool Connection::OnWritable(int fd) noexcept {
    if (closed_) return false;
    if (fd == upstream_fd_) TryFlush(client_fd_, upstream_fd_, c2u_);
    else if (fd == client_fd_) TryFlush(upstream_fd_, client_fd_, u2c_);
    return !closed_;
}

}  // namespace sluiced
