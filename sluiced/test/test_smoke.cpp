// Proves the toolchain works end to end: compiler, CMake, Catch2 fetch.
// Always wired into sluiced_test — see CMakeLists.txt. Keep this trivial;
// it exists to catch a broken build, not to test anything about sluiced.
#include <catch2/catch_test_macros.hpp>

TEST_CASE("toolchain is alive", "[smoke]") {
    REQUIRE(1 + 1 == 2);
}
