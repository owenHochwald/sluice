#include "sluiced/maglev.hpp"
#include <cstdint>
#include <limits>
#include <span>

namespace sluiced {

namespace {

std::uint64_t HashBackend(std::string_view address,
                          std::uint64_t salt) noexcept {
  std::uint64_t hash = 14695981039346656037ULL;

  for (unsigned char c : address) {
    hash ^= c;
    hash *= 1099511628211ULL;
  }

  hash ^= salt;
  hash *= 1099511628211ULL;

  return hash;
}

} // namespace

MaglevTable::MaglevTable(std::span<const MaglevBackend> backends,
                         std::size_t table_size) {
  backends_.assign(backends.begin(), backends.end());

  constexpr std::size_t EMPTY = std::numeric_limits<std::size_t>::max();

  const std::size_t n = backends_.size();

  std::vector<std::size_t> next(n, 0);

  std::vector<std::size_t> skip(n);

  std::vector<std::size_t> offset(n);

  table_.assign(table_size, EMPTY);

  for (std::size_t i = 0; i < n; ++i) {
    offset[i] = HashBackend(backends_[i].address, 0xba) % table_size;
    skip[i] = HashBackend(backends_[i].address, 0xbe) % (table_size - 1) + 1;
  }

  std::size_t filled = 0;

  while (filled < table_size) {
    for (std::size_t i = 0; i < n && filled < table_size; ++i) {
      std::size_t c = (offset[i] + next[i] * skip[i]) % table_size;

      ++next[i];

      if (table_[c] != EMPTY) {
        continue;
      }

      table_[c] = i;
      ++filled;
    }
  }
}

std::size_t MaglevTable::Lookup(std::uint64_t conn_hash) const noexcept {
  return table_[conn_hash % table_.size()];
}

std::size_t MaglevTable::TableSize() const noexcept { return table_.size(); }

const std::vector<MaglevBackend> &MaglevTable::Backends() const noexcept {
  return backends_;
}

} // namespace sluiced