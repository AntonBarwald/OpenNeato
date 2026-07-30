#ifndef ZONE_LABEL_MATCH_H
#define ZONE_LABEL_MATCH_H

#include <Arduino.h>
#include <vector>
#include "geometry.h"

// Label-based zone resolution (a prior firmware bug read a positional `zone` index instead
// of the frontend's `zones=<comma labels>`, so zone restriction had no effect on real
// hardware). Kept out of nav_geometry.h/geometry.h since this needs Arduino String.

// One parsed zone: its resolved label and polygon.
struct LabeledZone {
    String label;
    std::vector<Point> polygon;
};

// Mirrors zoneLabel() in frontend/src/views/history/guided-start.tsx; keep in lockstep.
// `idx` is 0-based; the synthesized fallback is 1-based ("Zone 1", "Zone 2", ...).
inline String zoneLabelFor(const String& explicitLabel, int idx) {
    String trimmed = explicitLabel;
    trimmed.trim();
    if (!trimmed.isEmpty())
        return trimmed;
    return "Zone " + String(idx + 1);
}

// Splits a comma-joined label list (the `zones` query param / persisted
// guidedZones field) into trimmed, non-empty labels.
inline std::vector<String> splitZoneLabels(const String& csv) {
    std::vector<String> out;
    int start = 0;
    int len = static_cast<int>(csv.length());
    while (start <= len) {
        int comma = csv.indexOf(',', start);
        String piece = (comma < 0) ? csv.substring(start) : csv.substring(start, comma);
        piece.trim();
        if (!piece.isEmpty())
            out.push_back(piece);
        if (comma < 0)
            break;
        start = comma + 1;
    }
    return out;
}

// Returns the union of matching polygons; `unresolvedLabel` is set to the first requested
// label matching no zone (empty if all resolved) — callers surface this as "zone not found".
inline std::vector<std::vector<Point>> resolveZoneLabels(const std::vector<LabeledZone>& zones,
                                                         const std::vector<String>& requestedLabels,
                                                         String& unresolvedLabel) {
    std::vector<std::vector<Point>> matched;
    unresolvedLabel = "";
    for (const auto& want: requestedLabels) {
        bool found = false;
        for (const auto& z: zones) {
            if (z.label == want) {
                matched.push_back(z.polygon);
                found = true;
                break;
            }
        }
        if (!found && unresolvedLabel.isEmpty())
            unresolvedLabel = want;
    }
    return matched;
}

#endif // ZONE_LABEL_MATCH_H
