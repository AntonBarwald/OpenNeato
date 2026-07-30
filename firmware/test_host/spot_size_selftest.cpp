// Host-only self-test for firmware/src/spot_size.h — written BEFORE that header exists, per
// the TDD requirement in docs/spec-spot-area-clean.md. Covers clampSpotDimension(), the pure
// function backing sized spot clean's Width/Height clamping. This file does not compile until
// spot_size.h is written (the actual RED).
//
// Run:
//   clang++ -std=c++17 -I firmware/src firmware/test_host/spot_size_selftest.cpp -o /tmp/spotsizetest && /tmp/spotsizetest

#include "spot_size.h"
#include <cstdio>

static int failures = 0;

static void expectEq(int actual, int expected, const char *name) {
    if (actual != expected) {
        std::printf("FAIL: %s (expected %d, got %d)\n", name, expected, actual);
        failures++;
    } else {
        std::printf("PASS: %s\n", name);
    }
}

int main() {
    expectEq(clampSpotDimension(0), -1, "0 -> -1 (no override)");
    expectEq(clampSpotDimension(-5), -1, "negative -> -1 (no override)");
    expectEq(clampSpotDimension(-1), -1, "explicit sentinel -1 -> -1 unchanged");
    expectEq(clampSpotDimension(50), 100, "below floor clamps up to 100");
    expectEq(clampSpotDimension(100), 100, "boundary: 100 passes through");
    expectEq(clampSpotDimension(250), 250, "mid-range passes through");
    expectEq(clampSpotDimension(400), 400, "boundary: 400 passes through");
    expectEq(clampSpotDimension(500), 400, "above ceiling clamps down to 400");

    if (failures == 0) {
        std::printf("\nAll checks passed\n");
        return 0;
    }
    std::printf("\n%d check(s) FAILED\n", failures);
    return 1;
}
