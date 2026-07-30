#ifndef MANUAL_CLEAN_MANAGER_H
#define MANUAL_CLEAN_MANAGER_H

#include <Arduino.h>
#include <functional>
#include "config.h"
#include "drop_safety.h"
#include "json_fields.h"
#include "loop_task.h"
#include "motion_safety.h"
#include "neato_serial.h"

// Manages the manual clean lifecycle: TestMode entry/exit, LDS rotation,
// continuous safety polling (bumpers + stall detection), and directional
// obstacle blocking.
//
// Flow:
//   enable()  → TestMode On → SetLDSRotation On → active (polling begins)
//   move()    → checks obstacles → SetMotor wheels (or rejects)
//   disable() → stop wheels → motors off → SetLDSRotation Off → TestMode Off
//
// Safety:
//   - Motion is classified once per move() call into a MotionKind (Forward/Backward/
//     RotateLeft/RotateRight) and every rule below is re-derived from that — see
//     motion_safety.h for the decision table this replaced the old overlapping
//     movingForward/movingBackward booleans with (they were both true for every
//     rotation, which both over-blocked rotation on any latch and made the
//     direction-aware bumper/drop guards dead code for every rotate).
//   - Bumper contact blocks forward movement; for rotation, blocks only the
//     direction that sweeps the touched corner/side further in
//   - Wheel lift blocks all movement and motors, no exception
//   - Stall detection: polls wheel load while moving; if load exceeds
//     MANUAL_STALL_LOAD_PCT for MANUAL_STALL_COUNT consecutive polls, stops
//     wheels and latches a stall flag blocking further movement in that
//     direction — but never blocks rotation (a torque event has no reliable
//     spatial meaning for a rotate)
//   - Drop/cliff detection: polls DropSensorLeft/Right; over MANUAL_DROP_THRESHOLD_MM
//     latches a sticky block on forward movement (direction-aware for rotation,
//     like bumpers) — cleared by reversing, or auto-clears after
//     MANUAL_DROP_CLEAR_COUNT consecutive clean polls (symmetric debounce)
//   - Client watchdog stops wheels if no API activity within timeout
//     (uses WebServer::lastApiActivity — any request keeps it alive)
//
// Note: LIDAR is NOT used for movement blocking. The turret sits near the back
// of the D-shape, making distance-to-body-edge calculations unreliable. Bumpers
// are the ground truth for collision detection. LIDAR data is still available
// for the frontend map visualization via GET /api/lidar.

class ManualCleanManager : public LoopTask {
public:
    ManualCleanManager(NeatoSerial& serial);

    // Enter or exit manual mode (enable=true → TestMode On + LDS start, false → shutdown).
    // Returns false if already in requested state or a transition is in progress (caller gets 503).
    // Callback fires when the transition completes.
    bool enable(bool enable, std::function<void(bool)> callback);

    // Send a wheel move command. Validates against current obstacle state.
    // Returns false immediately if not active or a move is already queued (caller gets 503).
    // left/right: distance in mm (positive = forward, negative = backward); speed: mm/s.
    // On rejection, lastBlockReason() carries the specific cause.
    bool move(int leftMM, int rightMM, int speedMMs, std::function<void(bool)> callback);

    // Same as move(), but skips wheel-load stall detection for this one move (bumper/wheel-lift/
    // drop safety stay fully active). For a bounded, one-shot move whose normal breakaway effort
    // trips the stall threshold — e.g. NavigationManager::beginUndock() — not for general driving.
    bool moveRelaxedStall(int leftMM, int rightMM, int speedMMs, std::function<void(bool)> callback);

    // Cause of the most recent move() rejection (kNone if the last move was allowed or none has
    // been attempted yet). Read right after a move()/moveRelaxedStall() callback fires with
    // ok=false, or after a synchronous `false` return, for typed attribution instead of a bare bool.
    MoveBlockReason lastBlockReason() const { return lastMoveBlockReason; }

    // Control cleaning motors (brush, vacuum, side brush).
    // Returns false immediately if not active (caller gets 503).
    bool setMotors(bool brush, bool vacuum, bool sideBrush, std::function<void(bool)> callback);

    bool isActive() const { return active; }

    // Suppress the 5s client watchdog while a nav feature owns manual mode (an unattended
    // route shouldn't be killed by "no HTTP request in 5s"). Reset to false on every fresh
    // enable(true) success so a forgotten un-suppress can't leave the next owner unprotected.
    void setClientWatchdogSuppressed(bool s) { clientWatchdogSuppressed = s; }

    // Update motor/safety settings from SettingsManager. Called at boot and on change.
    void setStallThreshold(int pct) { stallLoadPct = pct; }
    void setBrushRpm(int rpm) { brushRpm = rpm; }
    void setVacuumSpeed(int pct) { vacuumSpeedPct = pct; }
    void setSideBrushPower(int mw) { sideBrushMw = mw; }
    void setDropThreshold(int mm) { dropThresholdMm = mm; } // Not wired to a setting yet

    // Return current safety + motor state as JSON (no serial I/O — reads in-memory flags)
    String getStatusJson() const;

private:
    NeatoSerial& serial;
    bool active = false;
    bool enabling = false; // Transition in progress (enable sequence)
    unsigned long enablingStartMs = 0; // millis() when enable() started (for timeout recovery)
    bool disabling = false; // Transition in progress (disable sequence)

    // Current motor state (to avoid redundant commands on toggle)
    bool brushOn = false;
    bool vacuumOn = false;
    bool sideBrushOn = false;

    // Safety state — updated by continuous polling (physical sensors)
    bool bumperFrontLeft = false; // Left front/LDS bumper (blocks forward + right turn)
    bool bumperFrontRight = false; // Right front/LDS bumper (blocks forward + left turn)
    bool bumperSideLeft = false; // Left side bumper (blocks right turn only)
    bool bumperSideRight = false; // Right side bumper (blocks left turn only)
    bool wheelLifted = false; // Either wheel extended (robot picked up)

    // Stall-induced virtual bumpers — set by stall detection, cleared on reverse move or
    // auto-cleared after enough consecutive clean polls. Decision logic (debounce/escape/
    // auto-clear) lives in motion_safety.h so it's host-testable.
    StallLatchState stallLatch;

    // Drop/cliff sensor latch — sticky like stall, cleared on reverse move or auto-cleared.
    // Decision logic (debounce/fail-closed/sticky/auto-clear) lives in drop_safety.h so it's
    // host-testable.
    DropLatchState dropLatch;

    MoveBlockReason lastMoveBlockReason = MoveBlockReason::kNone;

    void tick() override; // Called every loop() iteration (intervalMs = 0)

    // Polling tickers (independent sub-timers inside tick())
    Ticker safetyTicker; // MANUAL_SAFETY_POLL_MS
    Ticker stallTicker; // MANUAL_STALL_POLL_MS
    bool watchdogStopped = false; // True if watchdog already sent stop
    bool clientWatchdogSuppressed = false; // True while a nav feature owns manual mode

    // Runtime motor/safety settings (defaults from config.h, updated by SettingsManager)
    int stallLoadPct = MANUAL_STALL_LOAD_PCT;
    int brushRpm = MANUAL_BRUSH_RPM;
    int vacuumSpeedPct = MANUAL_VACUUM_SPEED_PCT;
    int sideBrushMw = MANUAL_SIDE_BRUSH_POWER_MW;
    int dropThresholdMm = MANUAL_DROP_THRESHOLD_MM;

    // Stall detection — tracks wheel load while moving
    bool wheelsMoving = false; // True between move() and stop/stall/disable
    int lastCmdLeftMM = 0; // Last commanded left wheel distance
    int lastCmdRightMM = 0; // Last commanded right wheel distance
    bool stallRelaxedForCurrentMove = false; // True while the in-flight move used moveRelaxedStall()

    // Poll digital sensors for bumper/wheel-lift state
    void pollBumpers();

    // Poll motor odometry while wheels are moving to detect stalls
    void pollStall();

    // Poll drop/cliff sensors — fresh read every call, bypasses the analog cache
    void pollDrop();

    // Shared implementation behind move()/moveRelaxedStall().
    bool moveInternal(int leftMM, int rightMM, int speedMMs, std::function<void(bool)> callback, bool relaxedStall);

    // Check if a move command is safe given current obstacle state (motion_safety.h). Sets
    // lastMoveBlockReason. Returns true if the move is allowed, false if blocked.
    bool isMoveAllowed(int leftMM, int rightMM);

    // Stop all wheel movement immediately
    void stopWheels();

    // Turn off all cleaning motors
    void stopAllMotors();
};

#endif // MANUAL_CLEAN_MANAGER_H
