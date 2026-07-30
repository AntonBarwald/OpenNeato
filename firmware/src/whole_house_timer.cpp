#include "whole_house_timer.h"
#include "clean_timer_policy.h"

void WholeHouseTimer::arm(unsigned long minutes) {
    armed = true;
    startMs = millis();
    durationMinutes = minutes;
    armedGeneration = neato.currentCleanGeneration();
}

void WholeHouseTimer::cancel() {
    armed = false;
}

long WholeHouseTimer::remainingSec() const {
    if (!armed)
        return -1;
    return cleanTimerRemainingSec(startMs, millis(), durationMinutes);
}

void WholeHouseTimer::tick() {
    if (!armed)
        return;

    // A different clean has started since arm() -- ours is already over (or was never the one
    // running); forget about it rather than acting on someone else's clean.
    if (neato.currentCleanGeneration() != armedGeneration) {
        armed = false;
        return;
    }

    unsigned long gen = armedGeneration;
    neato.getState([this, gen](bool ok, const RobotState& state) {
        // Re-check: may have been cancelled, expired, or superseded while this (possibly
        // async, cache-miss) fetch was in flight.
        if (!armed || neato.currentCleanGeneration() != gen)
            return;
        if (ok && cleanTimerShouldDisarm(state.uiState.c_str())) {
            armed = false; // the clean we were armed for already ended on its own
            return;
        }
        if (cleanTimerExpired(startMs, millis(), durationMinutes)) {
            armed = false;
            neato.clean("dock", -1, -1, nullptr); // same SetEvent path as the Home button
        }
    });
}
