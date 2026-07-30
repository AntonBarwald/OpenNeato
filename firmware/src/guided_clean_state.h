#ifndef GUIDED_CLEAN_STATE_H
#define GUIDED_CLEAN_STATE_H

// Guided Clean state machine states (see navigation_manager.h for the state diagram). Kept
// zero-Arduino-dependency so guided_motor_policy.h's host test can include it.
enum GuidedCleanState {
    GUIDED_IDLE,
    GUIDED_VALIDATING, // Async dock-status check in flight before undocking
    GUIDED_UNDOCKING,
    GUIDED_ROTATING,
    GUIDED_DRIVING,
    GUIDED_ESCAPING, // Reverse + turn-away after a physical block, before geometric reroute
    GUIDED_CORRECTING,
    GUIDED_DOCKING,
    GUIDED_CHARGING,
    GUIDED_DONE,
    GUIDED_ERROR,
};

#endif // GUIDED_CLEAN_STATE_H
