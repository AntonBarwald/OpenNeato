#ifndef ESCAPE_POLICY_H
#define ESCAPE_POLICY_H

#include "motion_safety.h"

// Pure MoveBlockReason -> escape-motion table (careful-driving spec §3.2). Zero Arduino
// dependency, host-testable — see escape_policy_selftest.cpp. Reuses motion_safety.h's
// existing decision table rather than inventing new safety logic: every turn direction below
// is the direction motionSafetyCheck() already allows for that reason during a rotate.

enum class EscapeTurnDir { None, Left, Right };

// firstLegMm is signed: negative = reverse, positive = forward (only kStallRear's mirror
// case is positive). hasEscape=false means this reason has no defined escape — the caller
// falls through to the existing reroute/fail path unchanged.
// Explicit ctor: same gnu++11 aggregate-init caveat as motion_safety.h's MotionSafetyFlags.
struct EscapeMotion {
    bool hasEscape = false;
    int firstLegMm = 0;
    bool hasTurn = false;
    EscapeTurnDir turnDir = EscapeTurnDir::None;
    int rotateDeg = 0;

    EscapeMotion() = default;
    EscapeMotion(bool hasEscape_, int firstLegMm_, bool hasTurn_, EscapeTurnDir turnDir_, int rotateDeg_) :
        hasEscape(hasEscape_), firstLegMm(firstLegMm_), hasTurn(hasTurn_), turnDir(turnDir_), rotateDeg(rotateDeg_) {}
};

// reverseMm/rotateDeg are caller-supplied (config.h NAV_MGR_ESCAPE_* constants) so this stays
// pure/host-testable without a config dependency.
inline EscapeMotion escapeMotionFor(MoveBlockReason reason, int reverseMm, int rotateDeg) {
    switch (reason) {
        // Left-side contact: reverse, then turn right -- sweeps the left side away (the
        // relieving direction motionSafetyCheck's RotateRight case already allows).
        case MoveBlockReason::kBumperFrontLeft: // NOLINT(bugprone-branch-clone) -- distinct escape per case group
        case MoveBlockReason::kBumperSideLeft:
            return {true, -reverseMm, true, EscapeTurnDir::Right, rotateDeg};

        // Right-side contact: mirror -- reverse, then turn left.
        case MoveBlockReason::kBumperFrontRight:
        case MoveBlockReason::kBumperSideRight:
            return {true, -reverseMm, true, EscapeTurnDir::Left, rotateDeg};

        // Forward stall and LIDAR veto carry no lateral information -- reverse only, no turn.
        case MoveBlockReason::kLidarProximity:
        case MoveBlockReason::kStallFront:
            return {true, -reverseMm, false, EscapeTurnDir::None, 0};

        // Rear stall (only reachable if a future caller reverses outside an escape itself):
        // mirror of stall-front -- drive forward, no turn.
        case MoveBlockReason::kStallRear:
            return {true, reverseMm, false, EscapeTurnDir::None, 0};

        // Drop/cliff: reverse only, NEVER a turn even though a side is known -- a rotate could
        // sweep a wheel across the same edge from a different angle.
        case MoveBlockReason::kDropLeft:
        case MoveBlockReason::kDropRight:
            return {true, -reverseMm, false, EscapeTurnDir::None, 0};

        // No escape: airborne (kWheelLift), or not physical (kQueueFull/kNotActive/kNone) --
        // fall through to the existing reroute/fail path unchanged.
        case MoveBlockReason::kWheelLift:
        case MoveBlockReason::kQueueFull:
        case MoveBlockReason::kNotActive:
        case MoveBlockReason::kNone:
            break;
    }
    return {};
}

#endif // ESCAPE_POLICY_H
