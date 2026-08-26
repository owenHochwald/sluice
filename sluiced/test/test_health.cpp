// HealthChecker::ShouldPanic — the DP-X-03 threshold boundary.
#include <catch2/catch_test_macros.hpp>

#include "sluiced/health_checker.hpp"

using sluiced::HealthChecker;
using sluiced::HealthCheckerConfig;

TEST_CASE("ShouldPanic fires only above the configured fraction", "[health]") {
    HealthCheckerConfig cfg;  // panic_threshold defaults to 0.5
    HealthChecker hc(cfg);

    REQUIRE(hc.ShouldPanic(6, 10));   // 60% faulted -> panic
    REQUIRE_FALSE(hc.ShouldPanic(5, 10));  // exactly 50% -> not yet
    REQUIRE_FALSE(hc.ShouldPanic(0, 10));
    REQUIRE_FALSE(hc.ShouldPanic(0, 0));   // no backends -> never panic
}

TEST_CASE("ShouldPanic respects a custom threshold", "[health]") {
    HealthCheckerConfig cfg;
    cfg.panic_threshold = 0.8;
    HealthChecker hc(cfg);

    REQUIRE_FALSE(hc.ShouldPanic(7, 10));  // 70% < 80%
    REQUIRE(hc.ShouldPanic(9, 10));        // 90% > 80%
}
