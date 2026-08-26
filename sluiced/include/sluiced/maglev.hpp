#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace sluiced {

struct MaglevBackend {
  std::string address; // "ip:port", matches sluice.v1.Backend.address
};

class MaglevTable {
public:
  // table_size should be prime; SPEC.md DP-U-08 default is 65537.
  explicit MaglevTable(std::span<const MaglevBackend> backends,
                       std::size_t table_size = 65537);

  // O(1), lock-free, no allocation. conn_hash is a hash of the connection
  // 5-tuple (DP-E-04). Returns an index into Backends().
  [[nodiscard]] std::size_t Lookup(std::uint64_t conn_hash) const noexcept;

  [[nodiscard]] std::size_t TableSize() const noexcept;
  [[nodiscard]] const std::vector<MaglevBackend> &Backends() const noexcept;

private:
  std::vector<MaglevBackend> backends_;
  std::vector<std::size_t>
      table_; // table_[i in [0,table_size)] -> index into backends_
};

} // namespace sluiced
