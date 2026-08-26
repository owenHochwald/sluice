#pragma once
// Shared routing state: the atomically-swappable Maglev table plus the
// Backend objects it selects among. One Router is shared by every event
// loop. The config stream and health checker publish new tables off the
// forwarding path; loops only ever read, lock-free (DP-E-03, DP-U-09).

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "sluiced/backend.hpp"
#include "sluiced/maglev.hpp"

namespace sluiced {

// One immutable routing snapshot: the full backend set at a config version
// and a Maglev table built over just the eligible subset. Published with a
// single atomic store and reclaimed only once no loop still references it.
struct RoutingState {
  std::uint64_t version = 0;
  std::vector<std::shared_ptr<Backend>> backends;  // full set, stable objects
  std::vector<std::shared_ptr<Backend>> eligible;  // parallel to table_ indices
  std::unique_ptr<MaglevTable> table;              // null when eligible is empty
};

class Router {
public:
  explicit Router(std::size_t table_size = 65537, bool power_of_two_choices = false);

  // Replace the backend set (from the config stream). Preserves the existing
  // Backend object for any address that survives, so a membership change does
  // not reset health or counters for pods that stayed. Rebuilds and publishes.
  void SetBackends(std::uint64_t version, const std::vector<std::string>& addresses);

  // Recompute eligibility (healthy && !ejected && !drained), apply the panic
  // rule, rebuild the table, and publish. Called by the health checker after
  // it mutates health/ejection state, and by Drain/Undrain.
  void Republish(double panic_threshold = 0.5);

  // Pick a backend for a new connection by 5-tuple hash. Lock-free; returns
  // nullptr when no backend is eligible.
  std::shared_ptr<Backend> Select(std::uint64_t conn_hash) const noexcept;

  // For the health checker: the full current set (stable within a version).
  std::vector<std::shared_ptr<Backend>> Backends() const;

  // For the admin socket.
  std::shared_ptr<const RoutingState> Snapshot() const noexcept;
  std::uint64_t Version() const noexcept;

  bool Drain(const std::string& address, double panic_threshold = 0.5);
  bool Undrain(const std::string& address, double panic_threshold = 0.5);

private:
  // Rebuild the published snapshot from all_ under control_mu_.
  void RepublishLocked(double panic_threshold);

  std::size_t table_size_;
  bool power_of_two_choices_;

  mutable std::mutex control_mu_;  // guards all_ and version_; never on the data path
  std::vector<std::shared_ptr<Backend>> all_;
  std::uint64_t version_ = 0;

  std::atomic<std::shared_ptr<const RoutingState>> published_;
};

}  // namespace sluiced
