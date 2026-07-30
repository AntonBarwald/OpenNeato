// Host-only self-test for firmware/src/motion_safety.h — written BEFORE that header existed,
// per the TDD requirement for this fix. Covers the two-part motion-classification defect in
// ManualCleanManager::isMoveAllowed() (git show 6469d9c):
//
//   bool movingForward  = (leftMM > 0) || (rightMM > 0);
//   bool movingBackward = (leftMM < 0) || (rightMM < 0);
//
// A rotate-in-place commands opposite signs, so BOTH are true for every rotation:
//   (a) over-restrictive: any latched front-ish flag blocks ALL rotation forever (the
//       confirmed Guided Clean deadlock — a stall latch from normal undock effort froze
//       every subsequent rotate attempt).
//   (b) under-restrictive: `!movingBackward` is always false during a rotation, so the
//       side-bumper/diagonal-front-bumper guards are dead code for every rotate — a pressed
//       side bumper does NOT stop the robot rotating further into it today.
//
// `legacyIsMoveAllowed()` below is a verbatim (variable-renamed only) port of the currently
// shipped logic, kept standalone (no ManualCleanManager/Arduino dependency) so the two
// defects can be demonstrated empirically, independent of whether motion_safety.h exists yet.
// The `motion_safety.h`-backed assertions further down are the actual RED: that header does
// not exist yet, so this file does not compile until it's written with the fix in place.
//
// Run:
//   clang++ -std=c++17 -I firmware/src firmware/test_host/motion_safety_selftest.cpp -o /tmp/mstest && /tmp/mstest

#include "motion_safety.h"
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

// -- Legacy replica (the currently shipped, buggy logic) ---------------------------------
// Flags named 1:1 with ManualCleanManager's members. No wheelLifted/rearBlocked short-circuit
// variations here beyond what isMoveAllowed() itself has — this is a straight port.
struct LegacyFlags {
    bool wheelLifted = false;
    bool bumperFrontLeft = false;
    bool bumperFrontRight = false;
    bool bumperSideLeft = false;
    bool bumperSideRight = false;
    bool stallFront = false;
    bool stallRear = false;
    bool dropTriggeredLeft = false;
    bool dropTriggeredRight = false;
};

static bool legacyIsMoveAllowed(int leftMM, int rightMM, const LegacyFlags& flags) {
    if (flags.wheelLifted)
        return false;

    bool movingForward = (leftMM > 0) || (rightMM > 0);
    bool movingBackward = (leftMM < 0) || (rightMM < 0);
    bool turningLeft = (rightMM > leftMM);
    bool turningRight = (leftMM > rightMM);

    bool frontBlocked = flags.bumperFrontLeft || flags.bumperFrontRight || flags.stallFront ||
                        flags.dropTriggeredLeft || flags.dropTriggeredRight;
    bool rearBlocked = flags.stallRear;

    if (frontBlocked && movingForward)
        return false;
    if ((flags.bumperFrontLeft || flags.dropTriggeredLeft) && turningRight && !movingBackward)
        return false;
    if ((flags.bumperFrontRight || flags.dropTriggeredRight) && turningLeft && !movingBackward)
        return false;
    if (rearBlocked && movingBackward)
        return false;
    if (flags.bumperSideLeft && turningRight && !movingBackward)
        return false;
    if (flags.bumperSideRight && turningLeft && !movingBackward)
        return false;

    return true;
}

int main() {
    // ============================================================================
    // Part 1: prove the two defects are real in the currently shipped logic.
    // ============================================================================
    {
        // Defect (a): a rotate-in-place (opposite-sign wheels) with ONLY a stall latch set
        // is incorrectly refused — this is the confirmed Guided Clean deadlock. A forward
        // stall latch should say nothing about whether rotating in place is safe.
        LegacyFlags flags;
        flags.stallFront = true;
        bool allowed = legacyIsMoveAllowed(-100, 100, flags); // rotate, mag 100mm
        expectTrue(!allowed, "LEGACY BUG (a): stallFront alone incorrectly blocks a rotate (the confirmed deadlock)");
    }
    {
        // Defect (b): a pressed LEFT side bumper does not stop the robot continuing to
        // rotate further into it — `!movingBackward` is always false during rotation, so the
        // side-bumper guard is dead code. turningRight = leftMM > rightMM (rotate right,
        // sweeping the left side further in — exactly the direction that should be refused).
        LegacyFlags flags;
        flags.bumperSideLeft = true;
        bool allowed = legacyIsMoveAllowed(100, -100, flags); // rotate right, into the left bumper
        expectTrue(allowed, "LEGACY BUG (b): pressed left side bumper does NOT block rotating further into it (dead code)");
    }

    // ============================================================================
    // Part 2: the fixed behavior, via motion_safety.h's MotionKind classification.
    // Every assertion in this section is the actual RED before motion_safety.h exists,
    // and must pass once the real fix (not a legacy-preserving port) is implemented.
    // ============================================================================

    // -- classifyMotion() is mutually exclusive, unlike the old overlapping booleans -----
    expectTrue(classifyMotion(0, 0) == MotionKind::Stopped, "classify: zero/zero is Stopped");
    expectTrue(classifyMotion(100, 100) == MotionKind::Forward, "classify: both positive is Forward");
    expectTrue(classifyMotion(-100, -100) == MotionKind::Backward, "classify: both negative is Backward");
    expectTrue(classifyMotion(-100, 100) == MotionKind::RotateLeft, "classify: left<right, opposite signs is RotateLeft");
    expectTrue(classifyMotion(100, -100) == MotionKind::RotateRight, "classify: left>right, opposite signs is RotateRight");
    expectTrue(classifyMotion(50, 100) == MotionKind::Forward, "classify: unequal same-sign positive is still Forward (arc)");

    // -- (a) fixed: a stall latch alone never blocks either rotate direction -------------
    {
        MotionSafetyFlags f;
        f.stallFront = true;
        expectTrue(motionSafetyCheck(MotionKind::RotateLeft, f) == MoveBlockReason::kNone,
                   "FIX (a): stallFront alone allows RotateLeft");
        expectTrue(motionSafetyCheck(MotionKind::RotateRight, f) == MoveBlockReason::kNone,
                   "FIX (a): stallFront alone allows RotateRight");
    }
    {
        MotionSafetyFlags f;
        f.stallRear = true;
        expectTrue(motionSafetyCheck(MotionKind::RotateLeft, f) == MoveBlockReason::kNone,
                   "FIX (a): stallRear alone allows RotateLeft");
        expectTrue(motionSafetyCheck(MotionKind::RotateRight, f) == MoveBlockReason::kNone,
                   "FIX (a): stallRear alone allows RotateRight");
    }

    // -- (b) fixed: rotate INTO a pressed side bumper is refused, escape direction allowed -
    {
        MotionSafetyFlags f;
        f.bumperSideLeft = true;
        // RotateRight sweeps the left side further in -> must be refused.
        expectTrue(motionSafetyCheck(MotionKind::RotateRight, f) == MoveBlockReason::kBumperSideLeft,
                   "FIX (b): pressed left side bumper refuses RotateRight (sweeps left side further in)");
        // RotateLeft sweeps the left side away -> must remain allowed (the relieving direction).
        expectTrue(motionSafetyCheck(MotionKind::RotateLeft, f) == MoveBlockReason::kNone,
                   "FIX (b): pressed left side bumper still allows RotateLeft (relieving direction)");
    }
    {
        MotionSafetyFlags f;
        f.bumperSideRight = true;
        expectTrue(motionSafetyCheck(MotionKind::RotateLeft, f) == MoveBlockReason::kBumperSideRight,
                   "FIX (b): pressed right side bumper refuses RotateLeft (sweeps right side further in)");
        expectTrue(motionSafetyCheck(MotionKind::RotateRight, f) == MoveBlockReason::kNone,
                   "FIX (b): pressed right side bumper still allows RotateRight (relieving direction)");
    }
    {
        // Same direction-dependent rule for the front-corner bumpers during rotation.
        MotionSafetyFlags f;
        f.bumperFrontLeft = true;
        expectTrue(motionSafetyCheck(MotionKind::RotateRight, f) == MoveBlockReason::kBumperFrontLeft,
                   "FIX (b): pressed left front bumper refuses RotateRight");
        expectTrue(motionSafetyCheck(MotionKind::RotateLeft, f) == MoveBlockReason::kNone,
                   "FIX (b): pressed left front bumper still allows RotateLeft");
    }

    // -- wheel-lift blocks everything, no exception --------------------------------------
    {
        MotionSafetyFlags f;
        f.wheelLifted = true;
        expectTrue(motionSafetyCheck(MotionKind::Forward, f) == MoveBlockReason::kWheelLift, "wheel-lift blocks Forward");
        expectTrue(motionSafetyCheck(MotionKind::Backward, f) == MoveBlockReason::kWheelLift, "wheel-lift blocks Backward");
        expectTrue(motionSafetyCheck(MotionKind::RotateLeft, f) == MoveBlockReason::kWheelLift,
                   "wheel-lift blocks RotateLeft");
        expectTrue(motionSafetyCheck(MotionKind::RotateRight, f) == MoveBlockReason::kWheelLift,
                   "wheel-lift blocks RotateRight");
        // Even alongside other flags that would otherwise allow -- wheel-lift always wins.
        f.stallFront = true;
        expectTrue(motionSafetyCheck(MotionKind::RotateLeft, f) == MoveBlockReason::kWheelLift,
                   "wheel-lift wins over an allow-both-directions stall flag");
    }

    // -- forward blocked by front-ish flags; backward remains the escape ------------------
    {
        MotionSafetyFlags f;
        f.bumperFrontLeft = true;
        expectTrue(motionSafetyCheck(MotionKind::Forward, f) == MoveBlockReason::kBumperFrontLeft,
                   "front bumper blocks Forward");
        expectTrue(motionSafetyCheck(MotionKind::Backward, f) == MoveBlockReason::kNone,
                   "front bumper does not block Backward (escape)");
    }
    {
        MotionSafetyFlags f;
        f.stallRear = true;
        expectTrue(motionSafetyCheck(MotionKind::Backward, f) == MoveBlockReason::kStallRear, "rear stall blocks Backward");
        expectTrue(motionSafetyCheck(MotionKind::Forward, f) == MoveBlockReason::kNone,
                   "rear stall does not block Forward (escape)");
    }

    // -- careful-driving spec §1.4 gap: side bumper during straight Forward driving was
    // invisible to motionSafetyCheck (only front bumpers were checked) -- confirmed live by
    // log evidence (RSIDEBIT read 1 six times in one run, never once blocked a Forward move).
    // Backward stays the escape direction, same as the front-bumper/drop rule above.
    {
        MotionSafetyFlags f;
        f.bumperSideLeft = true;
        expectTrue(motionSafetyCheck(MotionKind::Forward, f) == MoveBlockReason::kBumperSideLeft,
                   "FIX (spec §1.4): pressed left side bumper blocks Forward");
        expectTrue(motionSafetyCheck(MotionKind::Backward, f) == MoveBlockReason::kNone,
                   "left side bumper does not block Backward (escape)");
    }
    {
        MotionSafetyFlags f;
        f.bumperSideRight = true;
        expectTrue(motionSafetyCheck(MotionKind::Forward, f) == MoveBlockReason::kBumperSideRight,
                   "FIX (spec §1.4): pressed right side bumper blocks Forward");
        expectTrue(motionSafetyCheck(MotionKind::Backward, f) == MoveBlockReason::kNone,
                   "right side bumper does not block Backward (escape)");
    }

    // -- drop-latch direction rules mirror bumpers (spatially anchored, not blanket-allowed) -
    {
        MotionSafetyFlags f;
        f.dropLeft = true;
        expectTrue(motionSafetyCheck(MotionKind::Forward, f) == MoveBlockReason::kDropLeft, "drop-left blocks Forward");
        expectTrue(motionSafetyCheck(MotionKind::RotateRight, f) == MoveBlockReason::kDropLeft,
                   "drop-left blocks RotateRight (sweeps the triggered corner further in)");
        expectTrue(motionSafetyCheck(MotionKind::RotateLeft, f) == MoveBlockReason::kNone,
                   "drop-left allows RotateLeft (sweeps the triggered corner away)");
    }
    {
        MotionSafetyFlags f;
        f.dropRight = true;
        expectTrue(motionSafetyCheck(MotionKind::Forward, f) == MoveBlockReason::kDropRight, "drop-right blocks Forward");
        expectTrue(motionSafetyCheck(MotionKind::RotateLeft, f) == MoveBlockReason::kDropRight,
                   "drop-right blocks RotateLeft (sweeps the triggered corner further in)");
        expectTrue(motionSafetyCheck(MotionKind::RotateRight, f) == MoveBlockReason::kNone,
                   "drop-right allows RotateRight (sweeps the triggered corner away)");
    }

    // -- stall latch: escape-move immediate clear ------------------------------------------
    {
        StallLatchState state;
        state.front = true;
        stallLatchClearOnEscape(state, MotionKind::Backward);
        expectTrue(!state.front, "stall escape: Backward clears stallFront");

        state = StallLatchState();
        state.rear = true;
        stallLatchClearOnEscape(state, MotionKind::Forward);
        expectTrue(!state.rear, "stall escape: Forward clears stallRear");

        // Rotation is neither escape direction -- must not clear either latch.
        state = StallLatchState();
        state.front = true;
        state.rear = true;
        stallLatchClearOnEscape(state, MotionKind::RotateLeft);
        expectTrue(state.front && state.rear, "stall escape: rotation clears neither latch");
    }

    // -- stall latch: set-side debounce ----------------------------------------------------
    {
        StallLatchState state;
        bool t1 = stallLatchApplyReading(state, StallReading{true, MotionKind::Forward}, 2);
        expectTrue(!t1, "stall debounce: 1st overloaded Forward poll does not trigger yet");
        bool t2 = stallLatchApplyReading(state, StallReading{true, MotionKind::Forward}, 2);
        expectTrue(t2 && state.front, "stall debounce: 2nd consecutive overloaded Forward poll (== threshold) triggers");
    }
    {
        // A rotate reading, however overloaded, never latches either side.
        StallLatchState state;
        for (int i = 0; i < 5; i++)
            stallLatchApplyReading(state, StallReading{true, MotionKind::RotateLeft}, 2);
        expectTrue(!state.front && !state.rear, "stall debounce: an overloaded rotate reading never latches");
    }

    // -- stall latch: symmetric debounced auto-clear (design-review §2) --------------------
    {
        StallLatchState state;
        state.front = true;
        state.rear = true;
        for (int i = 0; i < 2; i++)
            stallLatchApplyClear(state, /*overloaded=*/false, /*clearCountThreshold=*/3);
        expectTrue(state.front && state.rear, "stall auto-clear: below threshold, latches remain set");
        stallLatchApplyClear(state, false, 3);
        expectTrue(!state.front && !state.rear, "stall auto-clear: Nth consecutive clean poll (== threshold) clears both");
    }
    {
        StallLatchState state;
        state.front = true;
        stallLatchApplyClear(state, false, 3);
        stallLatchApplyClear(state, true, 3); // one overloaded poll resets the clear counter
        stallLatchApplyClear(state, false, 3);
        expectTrue(state.front, "stall auto-clear: an overloaded poll resets the clear-count run");
    }

    if (failures > 0) {
        std::printf("\n%d check(s) FAILED\n", failures);
        return 1;
    }
    std::printf("\nAll checks passed\n");
    return 0;
}
