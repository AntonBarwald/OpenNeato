#include "manual_clean_manager.h"
#include "web_server.h"

ManualCleanManager::ManualCleanManager(NeatoSerial& serial) : LoopTask(0), serial(serial) {
    TaskRegistry::add(this);
}

// -- Enable/disable lifecycle ------------------------------------------------

bool ManualCleanManager::enable(bool doEnable, std::function<void(bool)> callback) {
    if (doEnable) {
        if (active || enabling) {
            if (callback)
                callback(false);
            return false;
        }

        enabling = true;
        enablingStartMs = millis();
        LOG("MANUAL", "Enabling manual mode...");

        // Step 1: Enter TestMode
        serial.testMode(true, [this, callback](bool ok) {
            if (!ok) {
                LOG("MANUAL", "TestMode On failed");
                enabling = false;
                if (callback)
                    callback(false);
                return;
            }

            // Step 2: Start LDS rotation
            serial.setLdsRotation(true, [this, callback](bool ok) {
                if (!ok) {
                    LOG("MANUAL", "SetLDSRotation On failed, reverting TestMode");
                    serial.testMode(false, nullptr);
                    enabling = false;
                    if (callback)
                        callback(false);
                    return;
                }

                LOG("MANUAL", "Manual mode active");
                enabling = false;
                active = true;
                serial.setManualCleanActive(true);
                safetyTicker.reset(); // Force immediate first poll
                stallTicker.reset();
                watchdogStopped = false;
                // Fresh acquisition always starts unsuppressed; new owner re-suppresses itself.
                clientWatchdogSuppressed = false;

                // Reset safety state
                bumperFrontLeft = false;
                bumperFrontRight = false;
                bumperSideLeft = false;
                bumperSideRight = false;
                wheelLifted = false;

                // Reset stall detection
                wheelsMoving = false;
                stallRelaxedForCurrentMove = false;
                stallLatch = StallLatchState();

                // Reset drop/cliff sensor latch
                dropLatch = DropLatchState();

                // Reset motor state
                brushOn = false;
                vacuumOn = false;
                sideBrushOn = false;

                if (callback)
                    callback(true);
            });
        });
    } else {
        if (!active || disabling) {
            if (callback)
                callback(false);
            return false;
        }

        disabling = true;
        LOG("MANUAL", "Disabling manual mode...");

        // Step 1: Stop wheels immediately
        serial.setMotorWheels(0, 0, 0, [this, callback](bool) {
            // Step 2: Turn off cleaning motors (best-effort, don't block on failure)
            stopAllMotors();

            // Step 3: Stop LDS rotation
            serial.setLdsRotation(false, [this, callback](bool) {
                // Step 4: Exit TestMode
                serial.testMode(false, [this, callback](bool ok) {
                    LOG("MANUAL", "Manual mode disabled (%s)", ok ? "clean" : "TestMode Off failed");
                    active = false;
                    serial.setManualCleanActive(false);
                    disabling = false;
                    if (callback)
                        callback(ok);
                });
            });
        });
    }

    return true;
}

// -- Movement with safety check ----------------------------------------------

bool ManualCleanManager::move(int leftMM, int rightMM, int speedMMs, std::function<void(bool)> callback) {
    return moveInternal(leftMM, rightMM, speedMMs, callback, /*relaxedStall=*/false);
}

bool ManualCleanManager::moveRelaxedStall(int leftMM, int rightMM, int speedMMs, std::function<void(bool)> callback) {
    return moveInternal(leftMM, rightMM, speedMMs, callback, /*relaxedStall=*/true);
}

bool ManualCleanManager::moveInternal(int leftMM, int rightMM, int speedMMs, std::function<void(bool)> callback,
                                      bool relaxedStall) {
    if (!active) {
        lastMoveBlockReason = MoveBlockReason::kNotActive;
        return false;
    }

    // Zero move = explicit stop, always allowed (priority so it jumps the queue)
    if (leftMM == 0 && rightMM == 0) {
        wheelsMoving = false;
        lastMoveBlockReason = MoveBlockReason::kNone;
        serial.setMotorWheels(0, 0, 0, [this, callback](bool ok) {
            if (!ok)
                lastMoveBlockReason = MoveBlockReason::kQueueFull;
            if (callback)
                callback(ok);
        });
        return true;
    }

    if (!isMoveAllowed(leftMM, rightMM)) {
        LOG("MANUAL", "Move blocked: L=%d R=%d reason=%s", leftMM, rightMM,
            moveBlockReasonToString(lastMoveBlockReason));
        // Stop wheels to make sure robot isn't coasting from a previous command
        wheelsMoving = false;
        serial.setMotorWheels(0, 0, 0, nullptr);
        if (callback)
            callback(false);
        return true; // Request was accepted and handled (blocked), not a queue error
    }

    // Track movement for stall detection
    lastCmdLeftMM = leftMM;
    lastCmdRightMM = rightMM;
    stallRelaxedForCurrentMove = relaxedStall;
    if (!wheelsMoving) {
        // New movement — reset stall tracking
        wheelsMoving = true;
        stallLatch.overCount = 0;
    }

    lastMoveBlockReason = MoveBlockReason::kNone;
    // setMotorWheels internally enqueues at CRITICAL priority
    serial.setMotorWheels(leftMM, rightMM, speedMMs, [this, callback](bool ok) {
        if (!ok)
            lastMoveBlockReason = MoveBlockReason::kQueueFull;
        if (callback)
            callback(ok);
    });
    return true;
}

// -- Motor control -----------------------------------------------------------

bool ManualCleanManager::setMotors(bool brush, bool vacuum, bool sideBrush, std::function<void(bool)> callback) {
    if (!active)
        return false;

    // Track how many motor commands need to complete.
    // Use a raw pointer in a shared array to avoid std::make_shared.
    int *remaining = new int(0);
    bool *anyFailed = new bool(false);

    auto done = [remaining, anyFailed, callback]() {
        (*remaining)--;
        if (*remaining <= 0) {
            bool failed = *anyFailed;
            delete remaining;
            delete anyFailed;
            if (callback)
                callback(!failed);
        }
    };
    auto fail = [anyFailed, done](bool ok) {
        if (!ok)
            *anyFailed = true;
        done();
    };

    // Only send commands for motors that changed state
    if (brush != brushOn) {
        (*remaining)++;
        serial.setMotorBrush(brush ? brushRpm : 0, fail);
        brushOn = brush;
    }
    if (vacuum != vacuumOn) {
        (*remaining)++;
        serial.setMotorVacuum(vacuum, vacuumSpeedPct, fail);
        vacuumOn = vacuum;
    }
    if (sideBrush != sideBrushOn) {
        (*remaining)++;
        serial.setMotorSideBrush(sideBrush, sideBrushMw, fail);
        sideBrushOn = sideBrush;
    }

    // No changes needed — callback immediately and clean up
    if (*remaining == 0) {
        delete remaining;
        delete anyFailed;
        if (callback)
            callback(true);
    }

    return true;
}

// -- Loop (safety polling + watchdog) ----------------------------------------

void ManualCleanManager::tick() {
    // Recover from stuck enabling state — if the enable callback never fires
    // (e.g. serial queue was full when TestMode/LDS commands were enqueued),
    // reset after 10s so the user can retry instead of being locked out forever.
    if (enabling && enablingStartMs > 0 && millis() - enablingStartMs >= 10000) {
        LOG("MANUAL", "Enable timeout — resetting enabling flag after 10s");
        enabling = false;
        enablingStartMs = 0;
    }

    if (!active)
        return;

    // Safety polling — bumpers (skip if serial queue is more than half full
    // to prevent queue saturation that blocks all other commands including
    // TestMode entry and move commands — root cause of #18)
    if (safetyTicker.elapsed(MANUAL_SAFETY_POLL_MS) && serial.queueDepth() <= NEATO_QUEUE_MAX_SIZE / 2) {
        pollBumpers();
        pollDrop();
    }

    // Stall detection — poll motor odometry while wheels are moving
    if (wheelsMoving && stallTicker.elapsed(MANUAL_STALL_POLL_MS) && serial.queueDepth() <= NEATO_QUEUE_MAX_SIZE / 2) {
        pollStall();
    }

    // Client watchdog — stop wheels if frontend goes silent. Suppressed while a nav feature
    // owns manual mode; its own stale-pose watchdog is the safety net during that run instead.
    unsigned long lastActivity = WebServer::lastApiActivity;
    unsigned long now = millis();
    if (!watchdogStopped && !clientWatchdogSuppressed && lastActivity > 0 &&
        now - lastActivity >= MANUAL_CLIENT_TIMEOUT_MS) {
        LOG("MANUAL", "Client watchdog: no API activity for %lu ms, stopping wheels",
            (unsigned long) MANUAL_CLIENT_TIMEOUT_MS);
        stopWheels();
        watchdogStopped = true;
    }
}

// -- Safety polling ----------------------------------------------------------

void ManualCleanManager::pollBumpers() {
    // Safety polling uses HIGH priority to jump ahead of normal sensor polls
    serial.getDigitalSensors(
            [this](bool ok, const DigitalSensorData& d) {
                if (!ok || !active)
                    return;

                bool prevLift = wheelLifted;
                bool prevFrontL = bumperFrontLeft;
                bool prevFrontR = bumperFrontRight;
                bool prevSideL = bumperSideLeft;
                bool prevSideR = bumperSideRight;

                bumperFrontLeft = d.lFrontBit || d.lLdsBit;
                bumperFrontRight = d.rFrontBit || d.rLdsBit;
                bumperSideLeft = d.lSideBit;
                bumperSideRight = d.rSideBit;
                wheelLifted = d.leftWheelExtended || d.rightWheelExtended;

                // Log state changes and stop wheels on any new contact
                if (wheelLifted && !prevLift) {
                    LOG("MANUAL", "SAFETY: Wheel lifted — stopping all motors and wheels");
                    stopWheels();
                    stopAllMotors();
                }
                if (bumperFrontLeft && !prevFrontL) {
                    LOG("MANUAL", "SAFETY: Left front bumper contact");
                    stopWheels();
                }
                if (bumperFrontRight && !prevFrontR) {
                    LOG("MANUAL", "SAFETY: Right front bumper contact");
                    stopWheels();
                }
                if (bumperSideLeft && !prevSideL) {
                    LOG("MANUAL", "SAFETY: Left side bumper contact");
                    stopWheels();
                }
                if (bumperSideRight && !prevSideR) {
                    LOG("MANUAL", "SAFETY: Right side bumper contact");
                    stopWheels();
                }
            },
            PRIORITY_HIGH);
}

// -- Stall detection ---------------------------------------------------------

void ManualCleanManager::pollStall() {
    serial.getMotors(
            [this](bool ok, const MotorData& m) {
                if (!ok || !active || !wheelsMoving)
                    return;

                // Wheel load percentage spikes when motors fight an obstacle.
                // Either wheel exceeding the threshold counts as stalled.
                bool overloaded = (lastCmdLeftMM != 0 && m.leftWheelLoad >= stallLoadPct) ||
                                  (lastCmdRightMM != 0 && m.rightWheelLoad >= stallLoadPct);

                if (stallRelaxedForCurrentMove) {
                    // Bounded one-shot move (e.g. undock) — bumper/drop stay fully active,
                    // wheel-load stall is skipped for this move only, no latch bookkeeping.
                    return;
                }

                MotionKind kind = classifyMotion(lastCmdLeftMM, lastCmdRightMM);
                bool justTriggered =
                        stallLatchApplyReading(stallLatch, StallReading{overloaded, kind}, MANUAL_STALL_COUNT);
                stallLatchApplyClear(stallLatch, overloaded, MANUAL_STALL_CLEAR_COUNT);

                if (justTriggered) {
                    LOG("MANUAL", "STALL: wheel overload (L load=%d%% R load=%d%%, cmd L=%d R=%d)", m.leftWheelLoad,
                        m.rightWheelLoad, lastCmdLeftMM, lastCmdRightMM);
                    stopWheels();
                }
            },
            PRIORITY_HIGH);
}

// -- Drop/cliff sensor detection ----------------------------------------------

void ManualCleanManager::pollDrop() {
    serial.getBatteryAnalogHighPriority([this](bool ok, const BatteryAnalogData& d) {
        if (!active)
            return;

        DropReading reading;
        reading.usable = ok && d.dropSensorLeftMM >= 0 && d.dropSensorRightMM >= 0;
        if (reading.usable) {
            reading.overLeft = d.dropSensorLeftMM >= dropThresholdMm;
            reading.overRight = d.dropSensorRightMM >= dropThresholdMm;
        }

        bool justTriggered = dropLatchApplyReading(dropLatch, reading, MANUAL_DROP_COUNT, MANUAL_DROP_FAIL_COUNT);
        dropLatchApplyClear(dropLatch, reading, MANUAL_DROP_CLEAR_COUNT);
        if (!justTriggered)
            return;

        if (!reading.usable) {
            LOG("MANUAL", "SAFETY: %d consecutive drop-sensor read failures — treating as triggered (fail-closed)",
                dropLatch.failCount);
            stopWheels();
            return;
        }
        LOG("MANUAL", "SAFETY: Drop/cliff sensor triggered (L=%dmm R=%dmm threshold=%dmm)", d.dropSensorLeftMM,
            d.dropSensorRightMM, dropThresholdMm);
        stopWheels();
    });
}

// -- Movement safety logic ---------------------------------------------------

bool ManualCleanManager::isMoveAllowed(int leftMM, int rightMM) {
    MotionKind kind = classifyMotion(leftMM, rightMM);

    // Escape-move immediate clear, ahead of the check -- same "moving backward and not
    // forward"/"moving forward and not backward" rule as before, now unambiguous per-kind
    // instead of via two overlapping booleans. Rotation is neither escape direction.
    stallLatchClearOnEscape(stallLatch, kind);
    if (kind == MotionKind::Backward)
        dropLatchClearOnReverse(dropLatch);

    MotionSafetyFlags flags{wheelLifted,     bumperFrontLeft,         bumperFrontRight,
                            bumperSideLeft,  bumperSideRight,         stallLatch.front,
                            stallLatch.rear, dropLatch.triggeredLeft, dropLatch.triggeredRight};

    lastMoveBlockReason = motionSafetyCheck(kind, flags);
    return lastMoveBlockReason == MoveBlockReason::kNone;
}

// -- Status JSON (no serial I/O) ---------------------------------------------

String ManualCleanManager::getStatusJson() const {
    return fieldsToJson({
            {"active", active ? "true" : "false", FIELD_BOOL},
            {"brush", brushOn ? "true" : "false", FIELD_BOOL},
            {"vacuum", vacuumOn ? "true" : "false", FIELD_BOOL},
            {"sideBrush", sideBrushOn ? "true" : "false", FIELD_BOOL},
            {"lifted", wheelLifted ? "true" : "false", FIELD_BOOL},
            {"bumperFrontLeft", bumperFrontLeft ? "true" : "false", FIELD_BOOL},
            {"bumperFrontRight", bumperFrontRight ? "true" : "false", FIELD_BOOL},
            {"bumperSideLeft", bumperSideLeft ? "true" : "false", FIELD_BOOL},
            {"bumperSideRight", bumperSideRight ? "true" : "false", FIELD_BOOL},
            {"stallFront", stallLatch.front ? "true" : "false", FIELD_BOOL},
            {"stallRear", stallLatch.rear ? "true" : "false", FIELD_BOOL},
            {"dropTriggeredLeft", dropLatch.triggeredLeft ? "true" : "false", FIELD_BOOL},
            {"dropTriggeredRight", dropLatch.triggeredRight ? "true" : "false", FIELD_BOOL},
    });
}

// -- Motor helpers -----------------------------------------------------------

void ManualCleanManager::stopWheels() {
    wheelsMoving = false;
    serial.setMotorWheels(0, 0, 0, nullptr);
}

void ManualCleanManager::stopAllMotors() {
    if (brushOn) {
        serial.setMotorBrush(0, nullptr);
        brushOn = false;
    }
    if (vacuumOn) {
        serial.setMotorVacuum(false, 0, nullptr);
        vacuumOn = false;
    }
    if (sideBrushOn) {
        serial.setMotorSideBrush(false, 0, nullptr);
        sideBrushOn = false;
    }
}
