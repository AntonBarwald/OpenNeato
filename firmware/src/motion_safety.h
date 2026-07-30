#ifndef MOTION_SAFETY_H
#define MOTION_SAFETY_H

// Pure decision logic for ManualCleanManager::isMoveAllowed(), extracted so the motion
// classification + safety-flag rules are host-testable without Arduino/NeatoSerial — same
// seam pattern as drop_safety.h/nav_geometry.h. See motion_safety_selftest.cpp.
//
// Root cause this replaces: isMoveAllowed() used to derive movingForward/movingBackward as
// two independent booleans over the wheel pair. A rotate-in-place commands opposite signs,
// so BOTH were true for every rotation — over-blocking every rotate once a front flag
// latched (the confirmed deadlock), and making the direction-aware bumper/drop guards
// (guarded by `!movingBackward`) permanently dead code for every rotate (a live safety
// hole: a pressed side bumper did not stop the robot rotating further into it). Classifying
// motion into one mutually-exclusive MotionKind fixes both at once.

enum class MotionKind {
    Stopped, // leftMM == 0 && rightMM == 0
    Forward, // leftMM >= 0 && rightMM >= 0, not both zero
    Backward, // leftMM <= 0 && rightMM <= 0, not both zero
    RotateLeft, // opposite signs, rightMM > leftMM (CCW)
    RotateRight, // opposite signs, leftMM > rightMM (CW)
};

inline MotionKind classifyMotion(int leftMM, int rightMM) {
    if (leftMM == 0 && rightMM == 0)
        return MotionKind::Stopped;
    if (leftMM >= 0 && rightMM >= 0)
        return MotionKind::Forward;
    if (leftMM <= 0 && rightMM <= 0)
        return MotionKind::Backward;
    return (rightMM > leftMM) ? MotionKind::RotateLeft : MotionKind::RotateRight;
}

// Reported out of the safety check so callers can log/attribute the real cause instead of
// one opaque "blocked" bool. Priority when multiple gates fire simultaneously matches the
// order motionSafetyCheck() evaluates them (wheel lift, then front/bumper/drop, then stall).
enum class MoveBlockReason {
    kNone,
    kNotActive,
    kWheelLift,
    kBumperFrontLeft,
    kBumperFrontRight,
    kBumperSideLeft,
    kBumperSideRight,
    kStallFront,
    kStallRear,
    kDropLeft,
    kDropRight,
    kQueueFull,
    kLidarProximity,
};

inline const char *moveBlockReasonToString(MoveBlockReason r) {
    switch (r) {
        case MoveBlockReason::kNone: // NOLINT(bugprone-branch-clone) -- distinct string per case, not a clone
            return "none";
        case MoveBlockReason::kNotActive:
            return "not_active";
        case MoveBlockReason::kWheelLift:
            return "wheel_lift";
        case MoveBlockReason::kBumperFrontLeft:
            return "bumper_front_left";
        case MoveBlockReason::kBumperFrontRight:
            return "bumper_front_right";
        case MoveBlockReason::kBumperSideLeft:
            return "bumper_side_left";
        case MoveBlockReason::kBumperSideRight:
            return "bumper_side_right";
        case MoveBlockReason::kStallFront:
            return "stall_front";
        case MoveBlockReason::kStallRear:
            return "stall_rear";
        case MoveBlockReason::kDropLeft:
            return "drop_left";
        case MoveBlockReason::kDropRight:
            return "drop_right";
        case MoveBlockReason::kQueueFull:
            return "queue_full";
        case MoveBlockReason::kLidarProximity:
            return "lidar_proximity";
    }
    return "unknown";
}

// Snapshot of every safety flag isMoveAllowed() reasons over — mirrors ManualCleanManager's
// members 1:1 (post any latch-clear for this call) so tests can build it without the class.
// Explicit ctor: build here resolves to gnu++11, which needs a real converting ctor for
// brace-init on a struct with default member initializers (not an aggregate pre-C++14).
struct MotionSafetyFlags {
    bool wheelLifted = false;
    bool bumperFrontLeft = false;
    bool bumperFrontRight = false;
    bool bumperSideLeft = false;
    bool bumperSideRight = false;
    bool stallFront = false;
    bool stallRear = false;
    bool dropLeft = false;
    bool dropRight = false;

    MotionSafetyFlags() = default;
    MotionSafetyFlags(bool wheelLifted_, bool bumperFrontLeft_, bool bumperFrontRight_, bool bumperSideLeft_,
                      bool bumperSideRight_, bool stallFront_, bool stallRear_, bool dropLeft_, bool dropRight_) :
        wheelLifted(wheelLifted_), bumperFrontLeft(bumperFrontLeft_), bumperFrontRight(bumperFrontRight_),
        bumperSideLeft(bumperSideLeft_), bumperSideRight(bumperSideRight_), stallFront(stallFront_),
        stallRear(stallRear_), dropLeft(dropLeft_), dropRight(dropRight_) {}
};

// The decision table from the design review, transcribed directly:
//   wheelLifted             -> block everything, no exception (airborne wheel is never safe)
//   bumper/drop (spatial)   -> direction-dependent for rotation: block only the direction
//                              that sweeps the flagged corner/side further in, allow the
//                              relieving direction; forward/backward keep the straight-line
//                              rule (block the direction driving further into the flag)
//   stall (transient force) -> allow rotation in BOTH directions regardless of latch — a
//                              forward/backward torque event carries no reliable spatial
//                              meaning for a rotate; this is the confirmed-deadlock fix
inline MoveBlockReason motionSafetyCheck(MotionKind kind, const MotionSafetyFlags& f) {
    if (f.wheelLifted)
        return MoveBlockReason::kWheelLift;

    switch (kind) {
        case MotionKind::Stopped:
            return MoveBlockReason::kNone;

        case MotionKind::Forward:
            if (f.bumperFrontLeft)
                return MoveBlockReason::kBumperFrontLeft;
            if (f.bumperFrontRight)
                return MoveBlockReason::kBumperFrontRight;
            // Side contact while translating straight is real contact, not a rotation-only
            // concern (spec §1.4 -- confirmed live: RSIDEBIT tripped 6 times in one run,
            // never once blocked). These are raw per-poll reads (re-read every 500ms, not
            // latched), so a momentary graze while sliding past a wall self-clears by the
            // next poll -- this only blocks the instant contact is actually current.
            if (f.bumperSideLeft)
                return MoveBlockReason::kBumperSideLeft;
            if (f.bumperSideRight)
                return MoveBlockReason::kBumperSideRight;
            if (f.stallFront)
                return MoveBlockReason::kStallFront;
            if (f.dropLeft)
                return MoveBlockReason::kDropLeft;
            if (f.dropRight)
                return MoveBlockReason::kDropRight;
            return MoveBlockReason::kNone;

        case MotionKind::Backward:
            if (f.stallRear)
                return MoveBlockReason::kStallRear;
            return MoveBlockReason::kNone;

        case MotionKind::RotateRight: // NOLINT(bugprone-branch-clone) -- each kind checks distinct flags, not a clone
            // Left wheel sweeps further than right — rotating further INTO the left side.
            if (f.bumperFrontLeft)
                return MoveBlockReason::kBumperFrontLeft;
            if (f.bumperSideLeft)
                return MoveBlockReason::kBumperSideLeft;
            if (f.dropLeft)
                return MoveBlockReason::kDropLeft;
            return MoveBlockReason::kNone;

        case MotionKind::RotateLeft:
            // Right wheel sweeps further than left — rotating further INTO the right side.
            if (f.bumperFrontRight)
                return MoveBlockReason::kBumperFrontRight;
            if (f.bumperSideRight)
                return MoveBlockReason::kBumperSideRight;
            if (f.dropRight)
                return MoveBlockReason::kDropRight;
            return MoveBlockReason::kNone;
    }
    return MoveBlockReason::kNone;
}

// -- Stall latch: same debounced set/clear shape as drop_safety.h's DropLatchState, but
// keyed to commanded direction (front=forward, rear=backward) instead of a fixed corner —
// a stall is a transient force event, not spatially anchored.
struct StallLatchState {
    bool front = false;
    bool rear = false;
    int overCount = 0; // consecutive overloaded polls under a Forward/Backward command
    int clearCount = 0; // consecutive clean (non-overloaded) polls while moving, any direction
};

// One stall poll result: was wheel load over threshold, and what motion was it commanded
// under. A Rotate reading carries no reliable forward/backward meaning (the confirmed bug
// was latching stallFront from a command where one wheel was actually reversing) so it
// never sets either side, though a clean rotate reading still counts toward auto-clear
// below (low resistance is unambiguous regardless of direction).
// Explicit ctor: same gnu++11 aggregate-init caveat as MotionSafetyFlags above.
struct StallReading {
    bool overloaded = false;
    MotionKind commandedKind = MotionKind::Stopped;

    StallReading() = default;
    StallReading(bool overloaded_, MotionKind commandedKind_) :
        overloaded(overloaded_), commandedKind(commandedKind_) {}
};

// Returns true if this call newly latched a side (false->true transition) — the caller's
// cue to stop the wheels, mirroring dropLatchApplyReading()'s return contract.
inline bool stallLatchApplyReading(StallLatchState& state, const StallReading& reading, int overCountThreshold) {
    if (reading.commandedKind != MotionKind::Forward && reading.commandedKind != MotionKind::Backward) {
        state.overCount = 0;
        return false;
    }
    if (!reading.overloaded) {
        state.overCount = 0;
        return false;
    }
    state.overCount++;
    if (state.overCount >= overCountThreshold) {
        bool prevFront = state.front;
        bool prevRear = state.rear;
        if (reading.commandedKind == MotionKind::Forward)
            state.front = true;
        else
            state.rear = true;
        return (state.front && !prevFront) || (state.rear && !prevRear);
    }
    return false;
}

// Symmetric debounced auto-clear (design-review §2): an autonomous caller that never issues
// a pure reverse can't rely on stallLatchClearOnEscape() alone. After clearCountThreshold
// consecutive non-overloaded polls, clear both latches — a clean reading is evidence
// resistance is gone regardless of which direction is currently commanded.
inline void stallLatchApplyClear(StallLatchState& state, bool overloaded, int clearCountThreshold) {
    if (overloaded) {
        state.clearCount = 0;
        return;
    }
    state.clearCount++;
    if (state.clearCount >= clearCountThreshold) {
        state.front = false;
        state.rear = false;
    }
}

// Immediate reverse-move escape — the fast path, kept alongside the debounce above (not
// instead of it). Backward clears front (moving away from a forward-stall obstruction);
// Forward clears rear, symmetrically. Rotation is neither: it doesn't relieve either.
inline void stallLatchClearOnEscape(StallLatchState& state, MotionKind kind) {
    if (kind == MotionKind::Backward)
        state.front = false;
    else if (kind == MotionKind::Forward)
        state.rear = false;
}

#endif // MOTION_SAFETY_H
