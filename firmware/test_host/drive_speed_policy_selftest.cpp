// Host-only self-test for firmware/src/drive_speed_policy.h — written BEFORE that header
// exists, per the TDD requirement for this fix (careful-driving spec §4.3, high-confidence
// smoothing item: scale wheel speed proportionally to step size instead of always commanding
// the full speed then hard-stopping on arrival).
//
// Run:
//   clang++ -std=c++17 -I firmware/src firmware/test_host/drive_speed_policy_selftest.cpp -o /tmp/dsptest && /tmp/dsptest

#include "drive_speed_policy.h"
#include <cstdio>

static int failures = 0;

static void expectEq(int got, int want, const char *name) {
    if (got != want) {
        std::printf("FAIL: %s (got %d, want %d)\n", name, got, want);
        failures++;
    } else {
        std::printf("PASS: %s\n", name);
    }
}

int main() {
    // Full-magnitude step -> full speed (the common case: a max-length drive/rotate step).
    expectEq(scaledSpeedMms(500.0f, 500.0f, 80, 200), 200, "full step -> max speed");

    // Anything beyond full magnitude (shouldn't happen, defensive) clamps to max, not runaway.
    expectEq(scaledSpeedMms(600.0f, 500.0f, 80, 200), 200, "over-magnitude clamps to max speed");

    // Zero-length step -> floor speed, never zero (a near-zero move still needs to move).
    expectEq(scaledSpeedMms(0.0f, 500.0f, 80, 200), 80, "zero-length step -> min speed floor");

    // Negative magnitude (shouldn't happen, defensive) also clamps to the floor.
    expectEq(scaledSpeedMms(-10.0f, 500.0f, 80, 200), 80, "negative magnitude clamps to min speed");

    // Halfway step -> halfway between floor and ceiling.
    expectEq(scaledSpeedMms(250.0f, 500.0f, 80, 200), 140, "half step -> midpoint speed");

    // A short final-approach step (e.g. 50mm of a 500mm cap) reads as slow/controlled.
    expectEq(scaledSpeedMms(50.0f, 500.0f, 80, 200), 92, "short final step -> near-floor speed");

    // fullMagnitude <= 0 is a degenerate/defensive input (shouldn't occur -- NAV_MAX_DRIVE_STEP_M
    // and NAV_MAX_ROTATE_STEP_DEG are both positive constants) -- fail safe to max speed rather
    // than divide by zero or return something nonsensical.
    expectEq(scaledSpeedMms(10.0f, 0.0f, 80, 200), 200, "degenerate fullMagnitude<=0 falls back to max speed");

    if (failures > 0) {
        std::printf("\n%d check(s) FAILED\n", failures);
        return 1;
    }
    std::printf("\nAll checks passed\n");
    return 0;
}
