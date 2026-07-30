#include "zones_manager.h"
#include "config.h"
#include <SPIFFS.h>
#include <vector>

bool ZonesManager::extractEpoch(const String& sessionName, String& epoch) {
    String name = sessionName;
    if (name.endsWith(".jsonl.hs")) {
        name = name.substring(0, name.length() - 9); // strip ".jsonl.hs"
    } else if (name.endsWith(".jsonl")) {
        name = name.substring(0, name.length() - 6); // strip ".jsonl"
    } else {
        return false;
    }

    if (name.isEmpty())
        return false;

    // Bound the digit run: real epochs are 10 (seconds) to 13 (millis) digits.
    // Blocks pathological oversized names before they build a too-long SPIFFS path.
    if (name.length() > 13)
        return false;

    for (unsigned int i = 0; i < name.length(); i++) {
        char c = name.charAt(i);
        if (c < '0' || c > '9')
            return false;
    }

    epoch = name;
    return true;
}

String ZonesManager::pinPath(const String& epoch) {
    return String(HISTORY_DIR) + "/" + epoch + ".pin";
}

String ZonesManager::zonesPath(const String& epoch) {
    return String(ZONES_DIR) + "/" + epoch + ".json";
}

bool ZonesManager::isPinned(const String& sessionName) const {
    String epoch;
    if (!extractEpoch(sessionName, epoch))
        return false;
    return SPIFFS.exists(pinPath(epoch));
}

bool ZonesManager::pin(const String& sessionName) {
    String epoch;
    if (!extractEpoch(sessionName, epoch))
        return false;

    String path = pinPath(epoch);
    if (SPIFFS.exists(path))
        return true; // Already pinned

    File f = SPIFFS.open(path, FILE_WRITE);
    if (!f)
        return false;
    f.close();
    return true;
}

bool ZonesManager::unpin(const String& sessionName) {
    String epoch;
    if (!extractEpoch(sessionName, epoch))
        return false;

    String path = pinPath(epoch);
    if (!SPIFFS.exists(path))
        return true; // Already unpinned

    return SPIFFS.remove(path);
}

bool ZonesManager::hasZones(const String& sessionName) const {
    String epoch;
    if (!extractEpoch(sessionName, epoch))
        return false;
    return SPIFFS.exists(zonesPath(epoch));
}

String ZonesManager::getZones(const String& sessionName) const {
    String epoch;
    if (!extractEpoch(sessionName, epoch))
        return "";

    String path = zonesPath(epoch);
    if (!SPIFFS.exists(path))
        return "";

    File f = SPIFFS.open(path, FILE_READ);
    if (!f)
        return "";
    String content = f.readString();
    f.close();
    return content;
}

bool ZonesManager::setZones(const String& sessionName, const String& json, String& error) {
    error = "";

    String epoch;
    if (!extractEpoch(sessionName, epoch)) {
        error = "Invalid session name";
        return false;
    }

    if (json.length() > ZONES_MAX_BYTES) {
        error = "Zones payload too large";
        return false;
    }

    File f = SPIFFS.open(zonesPath(epoch), FILE_WRITE);
    if (!f) {
        error = "Failed to open zones file";
        return false;
    }
    f.write(reinterpret_cast<const uint8_t *>(json.c_str()), json.length());
    f.close();
    return true;
}

bool ZonesManager::deleteZones(const String& sessionName) {
    String epoch;
    if (!extractEpoch(sessionName, epoch))
        return false;

    String path = zonesPath(epoch);
    if (!SPIFFS.exists(path))
        return false;

    return SPIFFS.remove(path);
}

void ZonesManager::deleteAll() {
    // Remove all zone blobs
    File zonesRoot = SPIFFS.open(ZONES_DIR);
    if (zonesRoot && zonesRoot.isDirectory()) {
        std::vector<String> paths;
        File entry = zonesRoot.openNextFile();
        while (entry) {
            paths.push_back(String(entry.path()));
            entry = zonesRoot.openNextFile();
        }
        for (const auto& p: paths) {
            SPIFFS.remove(p);
        }
    }

    // Remove all pin markers from the history directory
    File histRoot = SPIFFS.open(HISTORY_DIR);
    if (histRoot && histRoot.isDirectory()) {
        std::vector<String> pinPaths;
        File entry = histRoot.openNextFile();
        while (entry) {
            String path = String(entry.path());
            if (path.endsWith(".pin"))
                pinPaths.push_back(path);
            entry = histRoot.openNextFile();
        }
        for (const auto& p: pinPaths) {
            SPIFFS.remove(p);
        }
    }
}
