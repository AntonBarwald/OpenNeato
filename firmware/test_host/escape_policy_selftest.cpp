// Host-only self-test for firmware/src/escape_policy.h — written BEFORE that header exists,
// per the TDD requirement for docs/spec-careful-driving.md §3/§6. Covers escapeMotionFor(),
// the pure MoveBlockReason -> escape-motion table. One assertion per MoveBlockReason value.
// This file does not compile until escape_policy.h is written (the actual RED).
//
// Run:
//   clang++ -std=c++17 -I firmware/src firmware/test_host/escape_policy_selftest.cpp -o /tmp/eptest && /tmp/eptest

#include "escape_policy.h"
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

static constexpr int kReverseMm = 150;
static constexpr int kRotateDeg = 30;

int main() {
    // -- Left-side contact: reverse, then turn RIGHT (sweeps the left side away) -----------
    {
        EscapeMotion m = escapeMotionFor(MoveBlockReason::kBumperFrontLeft, kReverseMm, kRotateDeg);
        expectTrue(m.hasEscape, "kBumperFrontLeft: has an escape");
        expectTrue(m.firstLegMm == -kReverseMm, "kBumperFrontLeft: first leg is a reverse (negative)");
        expectTrue(m.hasTurn && m.turnDir == EscapeTurnDir::Right, "kBumperFrontLeft: turns right (away from left contact)");
        expectTrue(m.rotateDeg == kRotateDeg, "kBumperFrontLeft: rotate magnitude passed through");
    }
    {
        EscapeMotion m = escapeMotionFor(MoveBlockReason::kBumperSideLeft, kReverseMm, kRotateDeg);
        expectTrue(m.hasEscape, "kBumperSideLeft: has an escape");
        expectTrue(m.firstLegMm == -kReverseMm, "kBumperSideLeft: first leg is a reverse");
        expectTrue(m.hasTurn && m.turnDir == EscapeTurnDir::Right, "kBumperSideLeft: turns right");
    }

    // -- Right-side contact: reverse, then turn LEFT (mirror) ------------------------------
    {
        EscapeMotion m = escapeMotionFor(MoveBlockReason::kBumperFrontRight, kReverseMm, kRotateDeg);
        expectTrue(m.hasEscape, "kBumperFrontRight: has an escape");
        expectTrue(m.firstLegMm == -kReverseMm, "kBumperFrontRight: first leg is a reverse");
        expectTrue(m.hasTurn && m.turnDir == EscapeTurnDir::Left, "kBumperFrontRight: turns left (away from right contact)");
    }
    {
        EscapeMotion m = escapeMotionFor(MoveBlockReason::kBumperSideRight, kReverseMm, kRotateDeg);
        expectTrue(m.hasEscape, "kBumperSideRight: has an escape");
        expectTrue(m.firstLegMm == -kReverseMm, "kBumperSideRight: first leg is a reverse");
        expectTrue(m.hasTurn && m.turnDir == EscapeTurnDir::Left, "kBumperSideRight: turns left");
    }

    // -- Forward stall: no lateral information -> reverse only, no turn -------------------
    {
        EscapeMotion m = escapeMotionFor(MoveBlockReason::kStallFront, kReverseMm, kRotateDeg);
        expectTrue(m.hasEscape, "kStallFront: has an escape");
        expectTrue(m.firstLegMm == -kReverseMm, "kStallFront: first leg is a reverse");
        expectTrue(!m.hasTurn, "kStallFront: no turn (no side information)");
    }

    // -- Rear stall: mirror of stall-front -- drive forward, no turn ----------------------
    {
        EscapeMotion m = escapeMotionFor(MoveBlockReason::kStallRear, kReverseMm, kRotateDeg);
        expectTrue(m.hasEscape, "kStallRear: has an escape");
        expectTrue(m.firstLegMm == kReverseMm, "kStallRear: first leg is forward (positive), mirroring stall-front");
        expectTrue(!m.hasTurn, "kStallRear: no turn");
    }

    // -- Drop sensors: reverse ONLY, never a turn, regardless of side ----------------------
    {
        EscapeMotion m = escapeMotionFor(MoveBlockReason::kDropLeft, kReverseMm, kRotateDeg);
        expectTrue(m.hasEscape, "kDropLeft: has an escape");
        expectTrue(m.firstLegMm == -kReverseMm, "kDropLeft: first leg is a reverse");
        expectTrue(!m.hasTurn, "kDropLeft: never turns, even though a side is known");
    }
    {
        EscapeMotion m = escapeMotionFor(MoveBlockReason::kDropRight, kReverseMm, kRotateDeg);
        expectTrue(m.hasEscape, "kDropRight: has an escape");
        expectTrue(m.firstLegMm == -kReverseMm, "kDropRight: first leg is a reverse");
        expectTrue(!m.hasTurn, "kDropRight: never turns");
    }

    // -- No escape: wheel-lift, queue-full, not-active, none -------------------------------
    {
        EscapeMotion m = escapeMotionFor(MoveBlockReason::kWheelLift, kReverseMm, kRotateDeg);
        expectTrue(!m.hasEscape, "kWheelLift: no escape (airborne, not a move-away situation)");
    }
    {
        EscapeMotion m = escapeMotionFor(MoveBlockReason::kQueueFull, kReverseMm, kRotateDeg);
        expectTrue(!m.hasEscape, "kQueueFull: no escape (not physical)");
    }
    {
        EscapeMotion m = escapeMotionFor(MoveBlockReason::kNotActive, kReverseMm, kRotateDeg);
        expectTrue(!m.hasEscape, "kNotActive: no escape (not physical)");
    }
    {
        EscapeMotion m = escapeMotionFor(MoveBlockReason::kNone, kReverseMm, kRotateDeg);
        expectTrue(!m.hasEscape, "kNone: no escape (nothing to escape from)");
    }

    if (failures > 0) {
        std::printf("\n%d check(s) FAILED\n", failures);
        return 1;
    }
    std::printf("\nAll checks passed\n");
    
    {
        // LIDAR veto: reverse only, never a turn (obstacle ahead, side unknown)
        EscapeMotion m = escapeMotionFor(MoveBlockReason::kLidarProximity, 150, 30);
        expectTrue(m.hasEscape, "lidar veto yields an escape");
        expectTrue(m.firstLegMm == -150, "lidar veto reverses");
        expectTrue(!m.hasTurn, "lidar veto does not turn");
    }

return 0;
}
