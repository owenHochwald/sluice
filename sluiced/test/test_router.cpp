// Router selection: eligibility (drain, ejection), the panic override, and
// that every eligible backend actually gets picked.
#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <set>
#include <string>
#include <vector>

#include "sluiced/router.hpp"

using sluiced::Router;

namespace {

std::vector<std::string> MakeAddrs(int n) {
    std::vector<std::string> a;
    for (int i = 0; i < n; ++i) a.push_back("10.0.0." + std::to_string(i) + ":9000");
    return a;
}

std::uint64_t Mix(std::uint64_t x) {
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33;
    return x;
}

std::set<std::string> Reached(const Router& r, int samples) {
    std::set<std::string> seen;
    for (int i = 0; i < samples; ++i) {
        auto b = r.Select(Mix(static_cast<std::uint64_t>(i)));
        if (b) seen.insert(b->address);
    }
    return seen;
}

}  // namespace

TEST_CASE("Router selects across every eligible backend", "[router]") {
    Router r;
    r.SetBackends(1, MakeAddrs(10));
    REQUIRE(Reached(r, 100000).size() == 10);
}

TEST_CASE("Router excludes drained and ejected backends", "[router]") {
    Router r;
    r.SetBackends(1, MakeAddrs(10));

    r.Drain("10.0.0.0:9000");
    auto seen = Reached(r, 100000);
    REQUIRE(seen.count("10.0.0.0:9000") == 0);
    REQUIRE(seen.size() == 9);

    // Eject one more via its atomic flag, then republish.
    for (auto& b : r.Backends()) {
        if (b->address == "10.0.0.1:9000") b->ejected.store(true);
    }
    r.Republish();
    seen = Reached(r, 100000);
    REQUIRE(seen.count("10.0.0.1:9000") == 0);
    REQUIRE(seen.size() == 8);
}

TEST_CASE("Router panic override reincludes a faulted majority", "[router]") {
    Router r;
    r.SetBackends(1, MakeAddrs(10));

    // Fault 6 of 10 (> 50%): the panic rule should serve all 10 anyway.
    int faulted = 0;
    for (auto& b : r.Backends()) {
        if (faulted++ < 6) b->ejected.store(true);
    }
    r.Republish(0.5);
    REQUIRE(Reached(r, 100000).size() == 10);
}

TEST_CASE("Router returns nullptr with no eligible backends", "[router]") {
    Router r;
    REQUIRE(r.Select(123) == nullptr);
}
