// Host-only self-test for firmware/src/spot_verify.h -- written BEFORE that header exists,
// per the repo's TDD convention (see spot_size_selftest.cpp). Covers isSpotStartMismatch(),
// the pure predicate backing the sized-spot-clean safety-net verification: a spot clean was
// requested but the robot's polled UI state shows a house clean running instead (the
// Clean-Width/Height-without-mode defect's signature). This file does not compile until
// spot_verify.h is written (the actual RED).
//
// Run:
//   clang++ -std=c++17 -I firmware/src firmware/test_host/spot_verify_selftest.cpp -o /tmp/spotverifytest && /tmp/spotverifytest

#include "spot_verify.h"
#include <cstdio>

static int failures = 0;

static void expectEq(bool actual, bool expected, const char *name) {
    if (actual != expected) {
        std::printf("FAIL: %s (expected %d, got %d)\n", name, expected, actual);
        failures++;
    } else {
        std::printf("PASS: %s\n", name);
    }
}

int main() {
    expectEq(isSpotStartMismatch("UIMGR_STATE_HOUSECLEANINGRUNNING"), true,
              "house running while spot requested -- mismatch");
    expectEq(isSpotStartMismatch("UIMGR_STATE_HOUSECLEANINGPAUSED"), true,
              "house paused while spot requested -- still a house clean, mismatch");
    expectEq(isSpotStartMismatch("UIMGR_STATE_SPOTCLEANINGRUNNING"), false,
              "spot running as requested -- no mismatch");
    expectEq(isSpotStartMismatch("UIMGR_STATE_SPOTCLEANINGPAUSED"), false,
              "spot paused -- no mismatch");
    expectEq(isSpotStartMismatch("UIMGR_STATE_STANDBY"), false,
              "idle (e.g. already finished before the check fired) -- no mismatch");
    expectEq(isSpotStartMismatch(""), false, "empty state -- no mismatch (fail closed, not alarmed)");
    expectEq(isSpotStartMismatch(nullptr), false, "null state -- no mismatch (fail closed, not alarmed)");

    if (failures == 0) {
        std::printf("\nAll checks passed\n");
        return 0;
    }
    std::printf("\n%d check(s) FAILED\n", failures);
    return 1;
}
