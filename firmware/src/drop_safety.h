#ifndef DROP_SAFETY_H
#define DROP_SAFETY_H

// Pure decision logic for the manual-clean cliff/drop-sensor backstop, extracted out of
// ManualCleanManager::pollDrop()/isMoveAllowed() so it's host-testable without
// Arduino/NeatoSerial — see firmware/test_host/drop_safety_selftest.cpp.

// Sticky per-side latch state, mirrored 1:1 with ManualCleanManager's
// dropTriggeredLeft/Right + dropOverCount/dropFailCount members.
struct DropLatchState {
    bool triggeredLeft = false;
    bool triggeredRight = false;
    int overCount = 0; // Consecutive over-threshold reads (debounce)
    int failCount = 0; // Consecutive unusable reads (fail-closed)
    int clearCount = 0; // Consecutive clean reads (debounce for auto-clear)
};

// One drop-sensor poll result. `usable` is false for the -1 sentinel (sensor field absent
// or the serial read itself failed).
struct DropReading {
    bool usable = false;
    bool overLeft = false;
    bool overRight = false;
};

// Applies one poll result to `state` in place. Returns true if this call newly latched a
// side (false->true transition) — the caller's cue to stop the wheels.
//
// Fail-closed: after `failCountThreshold` consecutive unusable reads, both sides latch
// regardless of overLeft/overRight — triggers exactly once, on the read where failCount
// reaches the threshold, not on every read past it.
// Debounce: overCount must reach `overCountThreshold` consecutive over-threshold reads
// before a side latches.
// Sticky: once a side latches it stays latched here — only dropLatchClearOnReverse() clears it.
inline bool dropLatchApplyReading(DropLatchState& state, const DropReading& reading, int overCountThreshold,
                                  int failCountThreshold) {
    if (!reading.usable) {
        state.failCount++;
        if (state.failCount == failCountThreshold) {
            state.triggeredLeft = true;
            state.triggeredRight = true;
            return true;
        }
        return false;
    }
    state.failCount = 0;

    if (reading.overLeft || reading.overRight)
        state.overCount++;
    else
        state.overCount = 0;

    if (state.overCount >= overCountThreshold) {
        bool prevLeft = state.triggeredLeft;
        bool prevRight = state.triggeredRight;
        if (reading.overLeft)
            state.triggeredLeft = true;
        if (reading.overRight)
            state.triggeredRight = true;
        return (state.triggeredLeft && !prevLeft) || (state.triggeredRight && !prevRight);
    }
    return false;
}

// Reverse-move clears both latches — mirrors isMoveAllowed()'s "moving backward and not
// forward" escape-move rule. Debounce/fail counters are untouched (only pollDrop() owns those).
inline void dropLatchClearOnReverse(DropLatchState& state) {
    state.triggeredLeft = false;
    state.triggeredRight = false;
}

// Symmetric debounced auto-clear (design-review §2): an autonomous caller (Guided Clean, the
// Nav POC) never issues a pure reverse, so a sticky-only-cleared-by-escape latch is otherwise
// permanent for it. After clearCountThreshold consecutive clean (usable, both sides under
// threshold) reads, clear whichever side(s) are latched — call once per poll, alongside (not
// instead of) dropLatchApplyReading(). A single over-threshold or unusable read resets the
// clear-count run, so this can't race a fresh trigger.
inline void dropLatchApplyClear(DropLatchState& state, const DropReading& reading, int clearCountThreshold) {
    if (!reading.usable || reading.overLeft || reading.overRight) {
        state.clearCount = 0;
        return;
    }
    state.clearCount++;
    if (state.clearCount >= clearCountThreshold) {
        state.triggeredLeft = false;
        state.triggeredRight = false;
    }
}

#endif // DROP_SAFETY_H
