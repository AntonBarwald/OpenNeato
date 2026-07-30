#ifndef ZONES_MANAGER_H
#define ZONES_MANAGER_H

#include <Arduino.h>

// Stores per-session no-go-line/zone geometry and pin state; synchronous on-demand SPIFFS
// I/O from web server handlers, not a LoopTask. Pin state is a marker file in HISTORY_DIR
// (not ZONES_DIR) so CleaningHistory::enforceLimits can protect it without knowing about
// zone geometry. Zones blob is opaque verbatim JSON, capped at ZONES_MAX_BYTES; firmware
// never parses it.
class ZonesManager {
public:
    // -- Pin state ------------------------------------------------------------
    bool isPinned(const String& sessionName) const;
    bool pin(const String& sessionName); // Idempotent; false only if sessionName is invalid
    bool unpin(const String& sessionName); // Idempotent

    // -- Zones blob (opaque, verbatim storage) --------------------------------
    bool hasZones(const String& sessionName) const;
    String getZones(const String& sessionName) const; // Empty string if none exists
    bool setZones(const String& sessionName, const String& json, String& error);
    bool deleteZones(const String& sessionName); // False if none stored or name is invalid

    // Wipe all zone blobs and pin markers — called when all history sessions are deleted.
    void deleteAll();

private:
    // Rejects anything not ending in a known session suffix with an all-digit prefix, to
    // avoid path traversal via attacker-controlled filenames from the HTTP layer.
    static bool extractEpoch(const String& sessionName, String& epoch);
    static String pinPath(const String& epoch);
    static String zonesPath(const String& epoch);
};

#endif // ZONES_MANAGER_H
