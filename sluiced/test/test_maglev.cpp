// TST-01 (distribution uniformity) and TST-02 (disruption bound) against
// sluiced::MaglevTable (include/sluiced/maglev.hpp). Written in full, but
// NOT wired into ../CMakeLists.txt yet — src/maglev.cpp doesn't exist, so
// this can't link. Once it does, uncomment this file's line in
// ../CMakeLists.txt's sluiced_test sources and these tests start counting.
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "sluiced/maglev.hpp"

using sluiced::MaglevBackend;
using sluiced::MaglevTable;

namespace {

std::vector<MaglevBackend> MakeBackends(int n) {
    std::vector<MaglevBackend> backends;
    backends.reserve(n);
    for (int i = 0; i < n; ++i) {
        backends.push_back({"10.0.0." + std::to_string(i) + ":9000"});
    }
    return backends;
}

// A cheap stand-in for "hash of the connection 5-tuple" — real callers hash
// actual 5-tuples, but Lookup only needs a uniformly distributed uint64_t
// input, so a simple mixing function is enough to exercise it here.
std::uint64_t Mix(std::uint64_t x) {
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33;
    x *= 0xc4ceb9fe1a85ec53ULL;
    x ^= x >> 33;
    return x;
}

}  // namespace

TEST_CASE("Maglev table distributes lookups roughly evenly", "[maglev]") {
    // TST-01: distribution uniformity within a documented tolerance.
    constexpr int kBackends = 10;
    constexpr int kSamples = 100'000;
    constexpr double kTolerance = 0.10; // each backend within +/-10% of the mean share

    auto backends = MakeBackends(kBackends);
    MaglevTable table(backends);

    std::vector<int> counts(kBackends, 0);
    for (int i = 0; i < kSamples; ++i) {
        counts[table.Lookup(Mix(static_cast<std::uint64_t>(i)))]++;
    }

    const double expected = static_cast<double>(kSamples) / kBackends;
    for (int i = 0; i < kBackends; ++i) {
        const double deviation = std::abs(counts[i] - expected) / expected;
        REQUIRE(deviation <= kTolerance);
    }
}

TEST_CASE("Removing one backend disrupts a bounded fraction of entries", "[maglev]") {
    // TST-02: removing one of N backends changes fewer than a documented
    // fraction of table entries. Maglev's headline property is that this is
    // close to 1/N, dramatically better than modulo hashing's ~100%; we
    // assert a generous multiple of 1/N so the test isn't brittle to the
    // exact implementation, while still catching a naive hash-mod approach.
    constexpr int kBackends = 20;

    auto before_backends = MakeBackends(kBackends);
    MaglevTable before(before_backends);

    auto after_backends = before_backends;
    after_backends.pop_back(); // remove exactly one backend
    MaglevTable after(after_backends);

    constexpr int kSamples = 50'000;
    int changed = 0;
    for (int i = 0; i < kSamples; ++i) {
        const auto hash = Mix(static_cast<std::uint64_t>(i));
        const std::string& before_addr = before.Backends()[before.Lookup(hash)].address;
        const std::string& after_addr = after.Backends()[after.Lookup(hash)].address;
        if (before_addr != after_addr) {
            ++changed;
        }
    }

    const double disruption = static_cast<double>(changed) / kSamples;
    const double tolerance = 3.0 / kBackends; // generous multiple of the ~1/N ideal
    REQUIRE(disruption <= tolerance);
}
