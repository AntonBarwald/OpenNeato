#ifndef MAINTENANCE_TRACKER_H
#define MAINTENANCE_TRACKER_H

#include <Arduino.h>
#include <Preferences.h>
#include "config.h"
#include "json_fields.h"
#include "loop_task.h"

class NeatoSerial;

// Hours-used-per-consumable, exposed read-only via toFields()/toJson().
struct MaintenanceData : public JsonSerializable {
    float brushHours = 0.0f;
    float filterHours = 0.0f;
    float sideBrushHours = 0.0f;
    float sensorHours = 0.0f;
    int brushIntervalHours = MAINT_BRUSH_INTERVAL_HOURS;
    int filterIntervalHours = MAINT_FILTER_INTERVAL_HOURS;
    int sideBrushIntervalHours = MAINT_SIDE_BRUSH_INTERVAL_HOURS;
    int sensorIntervalHours = MAINT_SENSOR_INTERVAL_HOURS;

    std::vector<Field> toFields() const override;
};

// Which consumable a reset targets (parsed from the ?item= query param).
enum MaintenanceItem {
    MAINT_ITEM_BRUSH,
    MAINT_ITEM_FILTER,
    MAINT_ITEM_SIDE_BRUSH,
    MAINT_ITEM_SENSORS,
    MAINT_ITEM_INVALID
};
MaintenanceItem parseMaintenanceItem(const String& name); // "brush"|"filter"|"sideBrush"|"sensors"

// Tracks cleaning-runtime-based consumable wear via one shared RAM-resident runtime
// accumulator plus four small per-item reset offsets; "hours used" = (accumulator -
// itemOffset) / 3600. Simpler than per-motor tracking (main brush/vacuum/side-brush run
// together during autonomous cleaning); manual clean's independent toggling slightly
// over-counts, acceptable for a v1 reminder, not a certified wear gauge.
// runtimeSeconds flushes to NVS only at session-end checkpoints (buffer in RAM, never write
// flash in a loop); reset offsets write immediately since they're user-triggered.
class MaintenanceTracker : public LoopTask {
public:
    MaintenanceTracker(NeatoSerial& neato, Preferences& prefs);

    // Must be called after prefs.begin() — the constructor runs during global static init,
    // before prefs.begin(), so reading NVS there would always see defaults.
    void begin();

    MaintenanceData get() const;

    // Sets this item's reset offset to the current runtime total ("hours used" becomes 0).
    bool reset(MaintenanceItem item);

private:
    void tick() override;

    NeatoSerial& neato;
    Preferences& prefs;

    unsigned long runtimeSeconds = 0; // RAM accumulator, loaded from NVS at boot
    unsigned long brushResetSeconds = 0;
    unsigned long filterResetSeconds = 0;
    unsigned long sideBrushResetSeconds = 0;
    unsigned long sensorResetSeconds = 0;

    String prevUiState;
    unsigned long lastTickMs = 0; // For computing elapsed seconds between ticks while active

    float hoursSince(unsigned long resetSeconds) const; // Clamped at 0 (see .cpp)
    void flushRuntime(); // Writes runtimeSeconds to NVS (called at session-end checkpoints only)
};

#endif // MAINTENANCE_TRACKER_H
