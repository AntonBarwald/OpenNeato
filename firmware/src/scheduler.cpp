#include "scheduler.h"
#include "data_logger.h"
#include "navigation_manager.h"
#include "notification_manager.h"
#include "zone_label_match.h"

Scheduler::Scheduler(SettingsManager& settings, SystemManager& system, NeatoSerial& serial, DataLogger& logger,
                     NavigationManager& navMgr, NotificationManager& notifMgr) :
    LoopTask(SCHEDULE_CHECK_INTERVAL_MS), settings(settings), system(system), serial(serial), dataLogger(logger),
    navMgr(navMgr), notifMgr(notifMgr) {
    TaskRegistry::add(this);
}

// C library: Sun=0, Mon=1 .. Sat=6
// Our schedule: Mon=0, Tue=1 .. Sun=6
int Scheduler::toSchedDay(int tmWday) {
    return (tmWday + 6) % 7;
}

void Scheduler::resetFiredGuards(int day) {
    if (day == firedDay)
        return;
    firedDay = day;
    firedAutoRestart = -1;
    for (int& fs: firedSlots)
        fs = -1;
}

bool Scheduler::isRobotIdle(const RobotState& state) const {
    return state.uiState == "UIMGR_STATE_IDLE" || state.uiState == "UIMGR_STATE_STANDBY";
}

bool Scheduler::isActionDue(int hour, int minute, int nowMins, int lastFiredMins, int& outSchedMins) {
    outSchedMins = hour * 60 + minute;
    int elapsed = nowMins - outSchedMins;
    if (elapsed < 0 || elapsed > SCHEDULE_WINDOW_MINS)
        return false;
    if (outSchedMins == lastFiredMins)
        return false;
    return true;
}

bool Scheduler::handleScheduledCleaning(const Settings& s, int day, int nowMins) {
    if (!s.scheduleEnabled)
        return false;

    const SchedDay& daySlots = s.sched[day];

    for (int si = 0; si < SCHEDULE_SLOTS_PER_DAY; si++) {
        const SchedSlot& slot = daySlots.slots[si];
        if (!slot.on)
            continue;

        int schedMins;
        if (!isActionDue(slot.hour, slot.minute, nowMins, firedSlots[si], schedMins))
            continue;

        String slotStr = String(schedMins / 60) + ":" + (schedMins % 60 < 10 ? "0" : "") + String(schedMins % 60);

        bool restartFirst = s.restartBeforeClean;
        serial.getState([this, si, day, schedMins, slotStr, restartFirst](bool ok, const RobotState& state) {
            if (!ok) {
                LOG("SCHED", "GetState failed, cannot check robot state for slot %s", slotStr.c_str());
                dataLogger.logGenericEvent("scheduler_state_error",
                                           {{"day", String(day), FIELD_INT}, {"slot", slotStr, FIELD_STRING}});
                return;
            }

            if (!isRobotIdle(state)) {
                LOG("SCHED", "Robot busy (%s), skipping slot %s", state.uiState.c_str(), slotStr.c_str());
                dataLogger.logGenericEvent("scheduler_skipped", {{"day", String(day), FIELD_INT},
                                                                 {"slot", slotStr, FIELD_STRING},
                                                                 {"reason", "busy", FIELD_STRING},
                                                                 {"state", state.uiState, FIELD_STRING}});
                firedSlots[si] = schedMins;
                return;
            }

            // No docked check here even for guided slots — that runs post-restart in
            // triggerGuidedClean() instead of on every check-interval tick.
            if (restartFirst) {
                LOG("SCHED", "Restarting robot before scheduled clean (day=%d slot=%d %s)", day, si, slotStr.c_str());
                dataLogger.logGenericEvent("scheduler_restart_before_clean",
                                           {{"day", String(day), FIELD_INT}, {"slot", slotStr, FIELD_STRING}});

                serial.powerControl("restart", [this, si, day, slotStr](bool okRestart) {
                    if (!okRestart) {
                        LOG("SCHED", "Restart before clean FAILED for slot %s", slotStr.c_str());
                        dataLogger.logGenericEvent("scheduler_restart_failed",
                                                   {{"day", String(day), FIELD_INT}, {"slot", slotStr, FIELD_STRING}});
                        return;
                    }
                    pendingCleanAfterRestart = true;
                    pendingCleanDay = day;
                    pendingCleanSlot = si;
                    restartIssuedAt = millis();
                });
            } else {
                triggerClean(day, si);
            }

            // Marked fired even if powerControl("restart") failed above (that branch only
            // logs and returns) — avoids retry-storming a broken restart every tick.
            firedSlots[si] = schedMins;
        });
        return true;
    }

    return false;
}

void Scheduler::handlePendingCleanAfterRestart() {
    if (!pendingCleanAfterRestart)
        return;

    if (millis() - restartIssuedAt > RESTART_BOOT_TIMEOUT_MS) {
        LOG("SCHED", "Robot boot timeout after restart, abandoning pending clean");
        dataLogger.logGenericEvent("scheduler_boot_timeout", {});
        clearPendingCleanAfterRestart();
        return;
    }

    serial.getState([this](bool ok, const RobotState& state) {
        if (!ok)
            return;

        if (!isRobotIdle(state))
            return;

        LOG("SCHED", "Robot ready after restart, triggering clean (day=%d slot=%d)", pendingCleanDay, pendingCleanSlot);
        dataLogger.logGenericEvent("scheduler_clean_after_restart", {{"day", String(pendingCleanDay), FIELD_INT}});

        triggerClean(pendingCleanDay, pendingCleanSlot);
        clearPendingCleanAfterRestart();
    });
}

void Scheduler::clearPendingCleanAfterRestart() {
    pendingCleanAfterRestart = false;
    pendingCleanDay = -1;
    pendingCleanSlot = -1;
    restartIssuedAt = 0;
}

void Scheduler::triggerClean(int day, int slotIndex) {
    const Settings& s = settings.get();
    const SchedSlot& slot = s.sched[day].slots[slotIndex];
    String slotStr = String(slot.hour) + ":" + (slot.minute < 10 ? "0" : "") + String(slot.minute);

    if (!s.scheduleEnabled || !slot.on) {
        LOG("SCHED", "Skipping clean, schedule changed (day=%d slot=%d %s)", day, slotIndex, slotStr.c_str());
        dataLogger.logGenericEvent("scheduler_skipped", {{"day", String(day), FIELD_INT},
                                                         {"slot", slotStr, FIELD_STRING},
                                                         {"reason", "schedule_changed", FIELD_STRING}});
        return;
    }

    // Also the pending-clean-after-restart resume path — no separate bookkeeping needed.
    switch (slot.mode) {
        case SCHED_MODE_HOUSE:
            triggerHouseClean(day, slotIndex, slot);
            return;
        case SCHED_MODE_SPOT:
            triggerSpotClean(day, slotIndex, slot);
            return;
        case SCHED_MODE_GUIDED:
            triggerGuidedClean(day, slotIndex, slot);
            return;
        default:
            LOG("SCHED", "Unknown schedule mode %d, skipping (day=%d slot=%d)", static_cast<int>(slot.mode), day,
                slotIndex);
            return;
    }
}

void Scheduler::triggerHouseClean(int day, int slotIndex, const SchedSlot& slot) {
    String slotStr = String(slot.hour) + ":" + (slot.minute < 10 ? "0" : "") + String(slot.minute);

    LOG("SCHED", "Triggering house clean (day=%d slot=%d %s)", day, slotIndex, slotStr.c_str());
    dataLogger.logGenericEvent("scheduler_trigger", {{"day", String(day), FIELD_INT}, {"slot", slotStr, FIELD_STRING}});

    serial.clean("house", 0, 0, [this, day, slotStr](bool ok) {
        LOG("SCHED", "Clean %s", ok ? "started" : "FAILED");
        if (!ok) {
            dataLogger.logGenericEvent("scheduler_trigger_failed",
                                       {{"day", String(day), FIELD_INT}, {"slot", slotStr, FIELD_STRING}});
        }
    });
}

void Scheduler::triggerSpotClean(int day, int slotIndex, const SchedSlot& slot) {
    String slotStr = String(slot.hour) + ":" + (slot.minute < 10 ? "0" : "") + String(slot.minute);

    LOG("SCHED", "Triggering spot clean (day=%d slot=%d %s)", day, slotIndex, slotStr.c_str());
    dataLogger.logGenericEvent("scheduler_spot_trigger",
                               {{"day", String(day), FIELD_INT}, {"slot", slotStr, FIELD_STRING}});

    serial.clean("spot", 0, 0, [this, day, slotStr](bool ok) {
        LOG("SCHED", "Spot clean %s", ok ? "started" : "FAILED");
        if (!ok) {
            dataLogger.logGenericEvent("scheduler_spot_trigger_failed",
                                       {{"day", String(day), FIELD_INT}, {"slot", slotStr, FIELD_STRING}});
        }
    });
}

void Scheduler::logAndMaybeNotify(const char *eventName, int day, const String& slotStr, const String& message) {
    LOG("SCHED", "%s", message.c_str());
    dataLogger.logGenericEvent(
            eventName,
            {{"day", String(day), FIELD_INT}, {"slot", slotStr, FIELD_STRING}, {"message", message, FIELD_STRING}});
    notifMgr.notifyEvent("warning", "Scheduled guided clean skipped", message);
}

// Reuses the same validation/start path as the interactive POST /api/guided route
// (navMgr.start()). isRobotIdle()/busy is already checked by handleScheduledCleaning()
// before triggerClean() is called, for all three modes — not duplicated here.
void Scheduler::triggerGuidedClean(int day, int slotIndex, const SchedSlot& slot) {
    String slotStr = String(slot.hour) + ":" + (slot.minute < 10 ? "0" : "") + String(slot.minute);

    if (!settings.get().guidedScheduleArmed) {
        logAndMaybeNotify("scheduler_guided_gate_off", day, slotStr,
                          "Scheduled guided clean skipped: unattended guided cleans are not enabled "
                          "(Settings > Schedule > 'Allow unattended guided cleans')");
        return;
    }
    if (slot.guidedSession.isEmpty()) {
        logAndMaybeNotify("scheduler_guided_no_session", day, slotStr,
                          "Scheduled guided clean skipped: no reference session configured");
        return;
    }

    // Docked + battery-floor preconditions, read from one GetCharger round-trip — not implied
    // by handleScheduledCleaning()'s isRobotIdle() check.
    serial.getCharger([this, day, slotIndex, slotStr, slot](bool ok, const ChargerData& c) {
        if (!ok) {
            logAndMaybeNotify("scheduler_guided_charger_error", day, slotStr,
                              "Scheduled guided clean skipped: could not read robot battery/dock state");
            return;
        }
        if (!c.extPwrPresent) {
            logAndMaybeNotify("scheduler_guided_not_docked", day, slotStr,
                              "Scheduled guided clean skipped: robot is not docked");
            return;
        }
        if (c.fuelPercent >= 0 && c.fuelPercent < NAV_MGR_SCHED_MIN_START_BATTERY_PCT) {
            logAndMaybeNotify("scheduler_guided_low_battery", day, slotStr,
                              "Scheduled guided clean skipped: battery too low to start scheduled guided clean, "
                              "will retry next scheduled time");
            return;
        }

        String error;
        if (!navMgr.start(slot.guidedSession, splitZoneLabels(slot.guidedZones), error)) {
            logAndMaybeNotify("scheduler_guided_start_failed", day, slotStr,
                              String("Scheduled guided clean skipped: ") + error);
            return;
        }

        LOG("SCHED", "Triggering guided clean (day=%d slot=%d %s session=%s)", day, slotIndex, slotStr.c_str(),
            slot.guidedSession.c_str());
        dataLogger.logGenericEvent("scheduler_guided_trigger", {{"day", String(day), FIELD_INT},
                                                                {"slot", slotStr, FIELD_STRING},
                                                                {"session", slot.guidedSession, FIELD_STRING}});
    });
}

void Scheduler::handleAutoRestart(const Settings& s, int day, int nowMins) {
    if (!s.autoRestartEnabled)
        return;

    int schedMins;
    if (!isActionDue(s.autoRestartHour, s.autoRestartMinute, nowMins, firedAutoRestart, schedMins))
        return;

    String slotStr =
            String(s.autoRestartHour) + ":" + (s.autoRestartMinute < 10 ? "0" : "") + String(s.autoRestartMinute);

    serial.getState([this, day, schedMins, slotStr](bool ok, const RobotState& state) {
        if (!ok) {
            LOG("SCHED", "GetState failed, cannot check robot state for auto restart %s", slotStr.c_str());
            dataLogger.logGenericEvent("auto_restart_state_error",
                                       {{"day", String(day), FIELD_INT}, {"slot", slotStr, FIELD_STRING}});
            return;
        }

        if (!isRobotIdle(state)) {
            LOG("SCHED", "Robot busy (%s), skipping auto restart %s", state.uiState.c_str(), slotStr.c_str());
            dataLogger.logGenericEvent("auto_restart_skipped", {{"day", String(day), FIELD_INT},
                                                                {"slot", slotStr, FIELD_STRING},
                                                                {"reason", "busy", FIELD_STRING},
                                                                {"state", state.uiState, FIELD_STRING}});
            firedAutoRestart = schedMins;
            return;
        }

        LOG("SCHED", "Triggering auto restart (%s)", slotStr.c_str());
        dataLogger.logGenericEvent("auto_restart_trigger",
                                   {{"day", String(day), FIELD_INT}, {"slot", slotStr, FIELD_STRING}});

        serial.powerControl("restart", [this, day, slotStr](bool okRestart) {
            LOG("SCHED", "Maintenance restart %s", okRestart ? "started" : "FAILED");
            if (!okRestart) {
                dataLogger.logGenericEvent("auto_restart_failed",
                                           {{"day", String(day), FIELD_INT}, {"slot", slotStr, FIELD_STRING}});
            }
        });

        firedAutoRestart = schedMins;
    });
}

void Scheduler::tick() {
    handlePendingCleanAfterRestart();

    const Settings& s = settings.get();

    // Get current local time (NTP preferred, robot fallback)
    time_t t = system.now();
    if (t <= 1700000000)
        return; // Clock not set yet

    struct tm tm;
    localtime_r(&t, &tm);

    int day = toSchedDay(tm.tm_wday);
    int nowMins = tm.tm_hour * 60 + tm.tm_min;
    resetFiredGuards(day);

    if (handleScheduledCleaning(s, day, nowMins))
        return;

    handleAutoRestart(s, day, nowMins);
}
