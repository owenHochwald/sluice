#pragma once
#include <array>
#include <cstddef>
#include <cstdint>

namespace sluiced {

class Connection {
public:
  // epoll_fd is the owning EventLoop's epoll instance — this Connection
  // registers client_fd and upstream_fd into it on construction and is
  // the only thing that ever calls epoll_ctl for those two fds afterward.
  Connection(int epoll_fd, int client_fd, int upstream_fd) noexcept;
  ~Connection();

  Connection(const Connection &) = delete;
  Connection &operator=(const Connection &) = delete;

  // Called by the owning EventLoop when client_fd or upstream_fd becomes
  // readable/writable. Returns false once the connection is closed in
  // both directions and may be destroyed.
  [[nodiscard]] bool OnReadable(int fd) noexcept;
  [[nodiscard]] bool OnWritable(int fd) noexcept;

  [[nodiscard]] int ClientFd() const noexcept { return client_fd_; }
  [[nodiscard]] int UpstreamFd() const noexcept { return upstream_fd_; }

private:
  // 16 KiB is a common TCP socket buffer size — big enough that a single
  // read() usually drains what the kernel has, small enough that two of
  // these per connection (32 KiB) stays cheap at thousands of connections
  // per core.
  static constexpr std::size_t kBufferSize = 16 * 1024;

  // One direction of the pipe: everything needed to resume a stalled
  // forward from exactly where it left off, and nothing else.
  struct Direction {
    std::array<std::byte, kBufferSize> buf;
    std::size_t len = 0;        // valid bytes currently in buf
    std::size_t offset = 0;     // bytes of buf[0, len) already written
    bool source_eof = false;    // source hit EOF or a read error
    bool sink_shutdown = false; // we've shutdown(SHUT_WR) the sink already
  };

  // read_fd is readable: pull bytes into dir and try to forward them.
  void TryRead(int source_fd, int sink_fd, Direction &dir) noexcept;
  // Write as much of dir's pending bytes to sink_fd as it will take right
  // now; called right after a read, and again on a resumed writable event.
  void TryFlush(int source_fd, int sink_fd, Direction &dir) noexcept;
  // Once a direction's source is at EOF and everything it sent has been
  // written out, propagate the half-close and check for full completion.
  void MaybeHalfClose(int sink_fd, Direction &dir) noexcept;
  // Both directions half-closed: close both fds, exactly once.
  void MaybeFinish() noexcept;

  // epoll interest bookkeeping — see connection.cpp for why each fd needs
  // its own independently-tracked bitmask.
  void Register(int fd) noexcept;  // first-time epoll_ctl(ADD), watch for readable
  void ArmRead(int fd) noexcept;
  void StopReading(int fd) noexcept;
  void ArmWrite(int fd) noexcept;
  void DisarmWrite(int fd) noexcept;
  void SetInterestBit(int fd, std::uint32_t bit, bool set) noexcept;

  int epoll_fd_;
  int client_fd_;
  int upstream_fd_;
  bool closed_ = false;

  // Current epoll event mask we've told the kernel to watch for each fd.
  std::uint32_t client_interest_ = 0;
  std::uint32_t upstream_interest_ = 0;

  Direction c2u_; // client -> upstream
  Direction u2c_; // upstream -> client
};

} // namespace sluiced
