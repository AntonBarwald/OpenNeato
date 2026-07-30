// Host-only self-test for firmware/src/guided_motor_policy.h — pins down
// guidedMotorsShouldRun() (which GuidedCleanState values run the cleaning motors),
// same host-testable-seam pattern as nav_geometry_selftest.cpp.
//
// Run:
//   clang++ -std=c++17 -I firmware/src firmware/test_host/guided_motor_policy_selftest.cpp -o /tmp/gmptest && /tmp/gmptest

#include "guided_motor_policy.h"
#include <cstdio>

static int failures = 0;

static void expectTrue(bool cond, const char *name) {
    if (!cond) {
        std::printf("FAIL: %s\n", name);
        failures++;
    } else {
        std::printf("PASS: %s\n", name);
    }
}

int main() {
    expectTrue(!guidedMotorsShouldRun(GUIDED_IDLE), "idle: motors off");
    expectTrue(!guidedMotorsShouldRun(GUIDED_VALIDATING), "validating: motors off (async dock check, not moving yet)");
    expectTrue(!guidedMotorsShouldRun(GUIDED_UNDOCKING), "undocking: motors off (don't vacuum the dock)");
    expectTrue(guidedMotorsShouldRun(GUIDED_ROTATING), "rotating: motors on");
    expectTrue(guidedMotorsShouldRun(GUIDED_DRIVING), "driving: motors on");
    expectTrue(guidedMotorsShouldRun(GUIDED_ESCAPING), "escaping: motors on (still recovering the route)");
    expectTrue(guidedMotorsShouldRun(GUIDED_CORRECTING), "correcting/reroute: motors on");
    expectTrue(!guidedMotorsShouldRun(GUIDED_DOCKING), "docking: motors off (must not vacuum while returning to dock)");
    expectTrue(!guidedMotorsShouldRun(GUIDED_CHARGING), "charging: motors off (must not vacuum while recharging)");
    expectTrue(!guidedMotorsShouldRun(GUIDED_DONE), "done: motors off");
    expectTrue(!guidedMotorsShouldRun(GUIDED_ERROR), "error: motors off");

    if (failures > 0) {
        std::printf("\n%d check(s) FAILED\n", failures);
        return 1;
    }
    std::printf("\nAll checks passed\n");
    return 0;
}
