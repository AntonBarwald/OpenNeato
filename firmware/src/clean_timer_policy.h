#ifndef CLEAN_TIMER_POLICY_H
#define CLEAN_TIMER_POLICY_H

#include <cstring>

// Pure deadline math for the whole-house-clean early-return timer. durationMinutes == 0 means
// "run until done" (the timer is not armed at all — callers should not construct one, but the
// functions treat it as never-expiring so a stray 0 fails safe). Uses unsigned subtraction
// throughout so it stays correct across millis() overflow (~49 days uptime), same convention as
// Ticker::elapsed() in loop_task.h.

inline bool cleanTimerExpired(unsigned long startMs, unsigned long nowMs, unsigned long durationMinutes) {
    if (durationMinutes == 0)
        return false;
    unsigned long durationMs = durationMinutes * 60000UL;
    return (nowMs - startMs) >= durationMs;
}

// Seconds remaining, floored at 0. Returns -1 for an unarmed (durationMinutes == 0) timer so
// callers can distinguish "no timer" from "timer just expired".
inline long cleanTimerRemainingSec(unsigned long startMs, unsigned long nowMs, unsigned long durationMinutes) {
    if (durationMinutes == 0)
        return -1;
    long elapsedSec = static_cast<long>((nowMs - startMs) / 1000UL);
    long totalSec = static_cast<long>(durationMinutes) * 60L;
    long remaining = totalSec - elapsedSec;
    return remaining > 0 ? remaining : 0;
}

// True if uiState shows the clean the timer was armed for has already ended -- naturally,
// by user stop/dock, error, or pickup -- i.e. it is no longer one of the "still running"
// states. Null/empty (a failed state read) fails safe: don't disarm on a transient blip.
inline bool cleanTimerShouldDisarm(const char *uiState) {
    if (!uiState || !uiState[0])
        return false;
    bool stillRunning =
            std::strstr(uiState, "CLEANINGRUNNING") != nullptr || std::strstr(uiState, "CLEANINGPAUSED") != nullptr ||
            std::strstr(uiState, "CLEANINGSUSPENDED") != nullptr || std::strstr(uiState, "DOCKING") != nullptr;
    return !stillRunning;
}

#endif // CLEAN_TIMER_POLICY_H
