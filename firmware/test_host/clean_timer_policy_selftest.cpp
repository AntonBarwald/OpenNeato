// Host-only self-test for firmware/src/clean_timer_policy.h — written BEFORE that header
// exists, per the TDD requirement for the whole-house early-return timer. Covers
// cleanTimerExpired()/cleanTimerRemainingSec(), the pure deadline math backing
// WholeHouseTimer. This file does not compile until clean_timer_policy.h is written
// (the actual RED).
//
// Run:
//   clang++ -std=c++17 -I firmware/src firmware/test_host/clean_timer_policy_selftest.cpp -o /tmp/ctptest && /tmp/ctptest

#include "clean_timer_policy.h"
#include <climits>
#include <cstdio>

static int failures = 0;

static void expectTrue(bool actual, const char *name) {
    if (!actual) {
        std::printf("FAIL: %s (expected true)\n", name);
        failures++;
    } else {
        std::printf("PASS: %s\n", name);
    }
}

static void expectFalse(bool actual, const char *name) {
    if (actual) {
        std::printf("FAIL: %s (expected false)\n", name);
        failures++;
    } else {
        std::printf("PASS: %s\n", name);
    }
}

static void expectEqLong(long actual, long expected, const char *name) {
    if (actual != expected) {
        std::printf("FAIL: %s (expected %ld, got %ld)\n", name, expected, actual);
        failures++;
    } else {
        std::printf("PASS: %s\n", name);
    }
}

int main() {
    // 1. Mid-run, well before deadline -> not expired.
    expectFalse(cleanTimerExpired(0UL, 60000UL, 30UL), "mid-run before deadline -> not expired");

    // 2. Exactly at the deadline boundary -> expired (>=, not >).
    expectTrue(cleanTimerExpired(0UL, 30UL * 60000UL, 30UL), "exact boundary -> expired");

    // 3. Past the deadline -> expired.
    expectTrue(cleanTimerExpired(0UL, 30UL * 60000UL + 1UL, 30UL), "past deadline -> expired");

    // 4. durationMinutes == 0 -> never expired, regardless of elapsed time.
    expectFalse(cleanTimerExpired(0UL, 0UL, 0UL), "durationMinutes 0 at t=0 -> never expired");
    expectFalse(cleanTimerExpired(0UL, ULONG_MAX, 0UL), "durationMinutes 0, huge elapsed -> never expired");

    // 5. Remaining seconds at start -> full durationMinutes * 60.
    expectEqLong(cleanTimerRemainingSec(1000UL, 1000UL, 30UL), 1800L, "remaining at start -> full duration");

    // 6. Remaining seconds past deadline -> floors at 0, never negative.
    expectEqLong(cleanTimerRemainingSec(0UL, 30UL * 60000UL + 5000UL, 30UL), 0L,
                 "remaining past deadline -> floors at 0");

    // 7. durationMinutes == 0 -> cleanTimerRemainingSec returns -1.
    expectEqLong(cleanTimerRemainingSec(0UL, 5000UL, 0UL), -1L, "unarmed timer -> remaining is -1");

    // 8. millis() wraparound: startMs near ULONG_MAX, nowMs small after wrapping. Unsigned
    // subtraction (nowMs - startMs) must still yield the correct positive elapsed (~100s here),
    // not a huge bogus value -- confirmed by it correctly tripping a 1-minute timer as expired.
    expectTrue(cleanTimerExpired(ULONG_MAX - 90000UL, 10000UL, 1UL), "wraparound elapsed computed correctly -> expired");

    // 9. Chosen ceiling (CLEAN_TIMER_MAX_MINUTES = 1440 = 24h) computes correctly -- nowhere
    // near the ~71583-minute point where durationMinutes * 60000UL overflows the 32-bit
    // unsigned long (2^32 / 60000 ~= 71582.78), confirming 1440 is a safe upper bound.
    expectFalse(cleanTimerExpired(0UL, 1440UL * 60000UL - 1000UL, 1440UL),
                "24h ceiling, 1s before deadline -> not expired");
    expectTrue(cleanTimerExpired(0UL, 1440UL * 60000UL, 1440UL), "24h ceiling, at deadline -> expired");
    expectEqLong(cleanTimerRemainingSec(0UL, 0UL, 1440UL), 1440L * 60L, "24h ceiling, remaining at start -> full duration");

    // 10. cleanTimerShouldDisarm() -- pure predicate backing WholeHouseTimer's "disarm when the
    // clean it was armed for ends, however it ends" requirement. True whenever the robot's
    // uiState is no longer one of the "still running" states.
    expectFalse(cleanTimerShouldDisarm("UIMGR_STATE_HOUSECLEANINGRUNNING"), "house running -> still active, don't disarm");
    expectFalse(cleanTimerShouldDisarm("UIMGR_STATE_HOUSECLEANINGPAUSED"), "house paused -> still active, don't disarm");
    expectFalse(cleanTimerShouldDisarm("UIMGR_STATE_CLEANINGSUSPENDED"), "mid-clean recharge -> still active, don't disarm");
    expectFalse(cleanTimerShouldDisarm("UIMGR_STATE_DOCKINGRUNNING"), "docking (mid-clean or self-docking) -> don't disarm yet");
    expectTrue(cleanTimerShouldDisarm("UIMGR_STATE_IDLE"), "idle -> clean ended (natural completion or user stop) -> disarm");
    expectTrue(cleanTimerShouldDisarm("UIMGR_STATE_STANDBY"), "standby -> clean ended -> disarm");
    expectTrue(cleanTimerShouldDisarm("UIMGR_STATE_ERROR"), "error/pickup -> clean ended -> disarm");
    expectFalse(cleanTimerShouldDisarm(""), "empty state (failed read) -> fail safe, don't disarm on a blip");
    expectFalse(cleanTimerShouldDisarm(nullptr), "null state -> fail safe, don't disarm on a blip");

    if (failures > 0) {
        std::printf("\n%d check(s) FAILED\n", failures);
        return 1;
    }
    std::printf("\nAll checks passed\n");
    return 0;
}
