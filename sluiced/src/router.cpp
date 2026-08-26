#include "sluiced/router.hpp"

#include <algorithm>

namespace sluiced {

namespace {

// A backend is a candidate for the table unless an operator has drained it.
bool Drained(const Backend& b) noexcept {
  return b.drained.load(std::memory_order_relaxed);
}

// Faulted = removed by the data plane's own health/ejection logic, as opposed
// to operator drain. This is the set the panic rule is allowed to override.
bool Faulted(const Backend& b) noexcept {
  return b.health_state.load(std::memory_order_relaxed) == HealthState::kUnhealthy ||
         b.ejected.load(std::memory_order_relaxed);
}

}  // namespace

Router::Router(std::size_t table_size, bool power_of_two_choices)
    : table_size_(table_size), power_of_two_choices_(power_of_two_choices) {}

void Router::SetBackends(std::uint64_t version, const std::vector<std::string>& addresses) {
  std::lock_guard<std::mutex> lock(control_mu_);

  // Preserve the object for any address that survives so health/counters
  // carry across a membership change; make a fresh one for a new address.
  std::vector<std::shared_ptr<Backend>> next;
  next.reserve(addresses.size());
  for (const auto& addr : addresses) {
    auto it = std::find_if(all_.begin(), all_.end(),
                           [&](const auto& b) { return b->address == addr; });
    if (it != all_.end()) {
      next.push_back(*it);
    } else {
      auto b = std::make_shared<Backend>();
      b->address = addr;
      next.push_back(std::move(b));
    }
  }
  all_ = std::move(next);
  version_ = version;
  RepublishLocked(0.5);
}

void Router::Republish(double panic_threshold) {
  std::lock_guard<std::mutex> lock(control_mu_);
  RepublishLocked(panic_threshold);
}

void Router::RepublishLocked(double panic_threshold) {
  auto state = std::make_shared<RoutingState>();
  state->version = version_;
  state->backends = all_;

  std::size_t candidates = 0;  // not drained
  std::size_t faulted = 0;     // not drained but removed by health/ejection
  for (const auto& b : all_) {
    if (Drained(*b)) continue;
    ++candidates;
    if (Faulted(*b)) ++faulted;
  }

  // DP-X-03: if the fault detectors want to remove more than the panic
  // fraction of the fleet, trust the fleet over the detectors and serve
  // every non-drained backend.
  const bool panic = candidates > 0 &&
                     static_cast<double>(faulted) > panic_threshold * static_cast<double>(candidates);

  for (const auto& b : all_) {
    if (Drained(*b)) continue;
    if (!panic && Faulted(*b)) continue;
    state->eligible.push_back(b);
  }

  if (!state->eligible.empty()) {
    std::vector<MaglevBackend> mb;
    mb.reserve(state->eligible.size());
    for (const auto& b : state->eligible) mb.push_back({b->address});
    state->table = std::make_unique<MaglevTable>(mb, table_size_);
  }

  published_.store(std::move(state), std::memory_order_release);
}

std::shared_ptr<Backend> Router::Select(std::uint64_t conn_hash) const noexcept {
  auto state = published_.load(std::memory_order_acquire);
  if (!state || !state->table || state->eligible.empty()) return nullptr;

  const std::size_t i = state->table->Lookup(conn_hash);
  if (!power_of_two_choices_) return state->eligible[i];

  // DP-O-02: probe a second bucket with a re-mixed hash and take whichever
  // backend currently carries fewer in-flight connections.
  std::uint64_t h2 = conn_hash * 0x9e3779b97f4a7c15ULL + 0x1ULL;
  const std::size_t j = state->table->Lookup(h2);
  const auto& a = state->eligible[i];
  const auto& b = state->eligible[j];
  return a->active_connections.load(std::memory_order_relaxed) <=
                 b->active_connections.load(std::memory_order_relaxed)
             ? a
             : b;
}

std::vector<std::shared_ptr<Backend>> Router::Backends() const {
  std::lock_guard<std::mutex> lock(control_mu_);
  return all_;
}

std::shared_ptr<const RoutingState> Router::Snapshot() const noexcept {
  return published_.load(std::memory_order_acquire);
}

std::uint64_t Router::Version() const noexcept {
  std::lock_guard<std::mutex> lock(control_mu_);
  return version_;
}

bool Router::Drain(const std::string& address, double panic_threshold) {
  std::lock_guard<std::mutex> lock(control_mu_);
  auto it = std::find_if(all_.begin(), all_.end(),
                         [&](const auto& b) { return b->address == address; });
  if (it == all_.end()) return false;
  (*it)->drained.store(true, std::memory_order_relaxed);
  RepublishLocked(panic_threshold);
  return true;
}

bool Router::Undrain(const std::string& address, double panic_threshold) {
  std::lock_guard<std::mutex> lock(control_mu_);
  auto it = std::find_if(all_.begin(), all_.end(),
                         [&](const auto& b) { return b->address == address; });
  if (it == all_.end()) return false;
  (*it)->drained.store(false, std::memory_order_relaxed);
  RepublishLocked(panic_threshold);
  return true;
}

}  // namespace sluiced
