#include "maintenance_tracker.h"
#include "neato_serial.h"

// True only while motors are plausibly spinning — narrower than
// CleaningHistory::isCleaningState(), which tracks session boundaries instead.
static bool isMotorActive(const String& uiState) {
    return uiState.indexOf("CLEANINGRUNNING") >= 0 || uiState.indexOf("MANUALCLEANING") >= 0;
}

MaintenanceTracker::MaintenanceTracker(NeatoSerial& neato, Preferences& prefs) :
    LoopTask(MAINT_POLL_MS), neato(neato), prefs(prefs) {
    TaskRegistry::add(this);
}

void MaintenanceTracker::begin() {
    runtimeSeconds = prefs.getULong(NVS_KEY_MAINT_RUNTIME, 0);
    brushResetSeconds = prefs.getULong(NVS_KEY_MAINT_BRUSH_RST, 0);
    filterResetSeconds = prefs.getULong(NVS_KEY_MAINT_FILTER_RST, 0);
    sideBrushResetSeconds = prefs.getULong(NVS_KEY_MAINT_SBRUSH_RST, 0);
    sensorResetSeconds = prefs.getULong(NVS_KEY_MAINT_SENSOR_RST, 0);
}

void MaintenanceTracker::tick() {
    // Skip while the serial queue is congested, mirroring ManualCleanManager's
    // safety-poll gate: this is a non-urgent maintenance counter and must never
    // add pressure that could delay motor/safety commands.
    if (neato.queueDepth() > NEATO_QUEUE_MAX_SIZE / 2)
        return;

    neato.getState([this](bool ok, const RobotState& state) {
        if (!ok)
            return;

        unsigned long now = millis();
        bool wasActive = isMotorActive(prevUiState);
        bool nowActive = isMotorActive(state.uiState);

        if (wasActive && lastTickMs != 0) {
            runtimeSeconds += (now - lastTickMs) / 1000;
        }

        // Session-end checkpoint: flush to NVS only on the active->inactive
        // edge, not on every tick (respects "never write flash in a loop").
        if (wasActive && !nowActive) {
            flushRuntime();
        }

        // Back off while idle — nothing accumulates when the motors are off, so
        // polling fast buys nothing and only adds serial traffic (mirrors
        // CleaningHistory's active/idle cadence split).
        setInterval(nowActive ? MAINT_POLL_MS : MAINT_POLL_IDLE_MS);

        prevUiState = state.uiState;
        lastTickMs = now;
    });
}

void MaintenanceTracker::flushRuntime() {
    prefs.putULong(NVS_KEY_MAINT_RUNTIME, runtimeSeconds);
}

// Clamped at zero: an offset can exceed the accumulator (reset mid-clean, then power lost
// before flush), and unsigned subtraction would otherwise wrap to a nonsense reading.
float MaintenanceTracker::hoursSince(unsigned long resetSeconds) const {
    if (runtimeSeconds <= resetSeconds)
        return 0.0f;
    return static_cast<float>(runtimeSeconds - resetSeconds) / 3600.0f;
}

MaintenanceData MaintenanceTracker::get() const {
    MaintenanceData d;
    d.brushHours = hoursSince(brushResetSeconds);
    d.filterHours = hoursSince(filterResetSeconds);
    d.sideBrushHours = hoursSince(sideBrushResetSeconds);
    d.sensorHours = hoursSince(sensorResetSeconds);
    return d;
}

bool MaintenanceTracker::reset(MaintenanceItem item) {
    switch (item) {
        case MAINT_ITEM_BRUSH:
            brushResetSeconds = runtimeSeconds;
            prefs.putULong(NVS_KEY_MAINT_BRUSH_RST, brushResetSeconds);
            return true;
        case MAINT_ITEM_FILTER:
            filterResetSeconds = runtimeSeconds;
            prefs.putULong(NVS_KEY_MAINT_FILTER_RST, filterResetSeconds);
            return true;
        case MAINT_ITEM_SIDE_BRUSH:
            sideBrushResetSeconds = runtimeSeconds;
            prefs.putULong(NVS_KEY_MAINT_SBRUSH_RST, sideBrushResetSeconds);
            return true;
        case MAINT_ITEM_SENSORS:
            sensorResetSeconds = runtimeSeconds;
            prefs.putULong(NVS_KEY_MAINT_SENSOR_RST, sensorResetSeconds);
            return true;
        default:
            return false;
    }
}

std::vector<Field> MaintenanceData::toFields() const {
    return {
            {"brushHours", String(brushHours, 1), FIELD_FLOAT},
            {"filterHours", String(filterHours, 1), FIELD_FLOAT},
            {"sideBrushHours", String(sideBrushHours, 1), FIELD_FLOAT},
            {"sensorHours", String(sensorHours, 1), FIELD_FLOAT},
            {"brushIntervalHours", String(brushIntervalHours), FIELD_INT},
            {"filterIntervalHours", String(filterIntervalHours), FIELD_INT},
            {"sideBrushIntervalHours", String(sideBrushIntervalHours), FIELD_INT},
            {"sensorIntervalHours", String(sensorIntervalHours), FIELD_INT},
    };
}

MaintenanceItem parseMaintenanceItem(const String& name) {
    if (name == "brush")
        return MAINT_ITEM_BRUSH;
    if (name == "filter")
        return MAINT_ITEM_FILTER;
    if (name == "sideBrush")
        return MAINT_ITEM_SIDE_BRUSH;
    if (name == "sensors")
        return MAINT_ITEM_SENSORS;
    return MAINT_ITEM_INVALID;
}
