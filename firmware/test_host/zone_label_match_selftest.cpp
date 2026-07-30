// Host-only self-test for firmware/src/zone_label_match.h — compiles against the Arduino
// shim, same pattern as point_array_json_selftest.cpp. Covers resolving a request's zone
// LABELS (not positional index) against saved zones, including "Zone N" fallback and the
// unresolved-label error path.
//
// Run:
//   clang++ -std=c++17 -I firmware/test_host/arduino_shim -I firmware/src \
//       firmware/test_host/zone_label_match_selftest.cpp -o /tmp/zltest && /tmp/zltest

#include "zone_label_match.h"
#include <cstdio>

static int failures = 0;

static void expectTrue(bool cond, const char *name) {
    if (!cond) {
        std::printf("FAIL: %s\n", name);
        failures++;
    } else {
        std::printf("PASS: %s\n", name);
    }
}

static bool strEq(const String& a, const char *b) {
    return a == b;
}

int main() {
    // -- zoneLabelFor: explicit label wins, trimmed ---------------------------
    expectTrue(strEq(zoneLabelFor("  Kitchen  ", 0), "Kitchen"), "zoneLabelFor: trims an explicit label");
    // -- zoneLabelFor: empty/whitespace-only falls back to synthesized "Zone N"
    expectTrue(strEq(zoneLabelFor("", 0), "Zone 1"), "zoneLabelFor: empty label -> synthesized Zone 1 (0-based idx 0)");
    expectTrue(strEq(zoneLabelFor("   ", 2), "Zone 3"), "zoneLabelFor: whitespace-only label -> synthesized Zone 3");

    // -- splitZoneLabels: comma-joined, trims, drops empties ------------------
    {
        auto labels = splitZoneLabels("Kitchen, Hallway ,,Living Room");
        expectTrue(labels.size() == 3, "splitZoneLabels: drops the empty middle entry");
        expectTrue(labels.size() == 3 && strEq(labels[0], "Kitchen"), "splitZoneLabels: first label trimmed");
        expectTrue(labels.size() == 3 && strEq(labels[1], "Hallway"), "splitZoneLabels: second label trimmed");
        expectTrue(labels.size() == 3 && strEq(labels[2], "Living Room"), "splitZoneLabels: third label preserved");
    }
    // -- splitZoneLabels: empty input -> empty list (whole house / no filter) -
    {
        auto labels = splitZoneLabels("");
        expectTrue(labels.empty(), "splitZoneLabels: empty csv -> empty label list");
    }

    // -- resolveZoneLabels: matches by explicit label, union of polygons -----
    {
        std::vector<LabeledZone> zones = {
                {"Kitchen", {{0, 0}, {1, 0}, {1, 1}, {0, 1}}},
                {"Hallway", {{2, 2}, {3, 2}, {3, 3}, {2, 3}}},
                {"Zone 3", {{5, 5}, {6, 5}, {6, 6}, {5, 6}}}, // never explicitly labeled, matches by synthesized name
        };
        String unresolved;
        auto matched = resolveZoneLabels(zones, {String("Kitchen"), String("Hallway")}, unresolved);
        expectTrue(unresolved.isEmpty(), "resolveZoneLabels: all requested labels resolve -> no unresolved label");
        expectTrue(matched.size() == 2, "resolveZoneLabels: union of two matched zones has 2 polygons");
    }

    // -- resolveZoneLabels: a label that matches nothing is reported ---------
    {
        std::vector<LabeledZone> zones = {{"Kitchen", {{0, 0}, {1, 0}, {1, 1}, {0, 1}}}};
        String unresolved;
        auto matched = resolveZoneLabels(zones, {String("Kitchen"), String("Garage")}, unresolved);
        expectTrue(strEq(unresolved, "Garage"), "resolveZoneLabels: unmatched label surfaced for the caller to error on");
        expectTrue(matched.size() == 1, "resolveZoneLabels: still returns the zones that DID match");
    }

    // -- resolveZoneLabels: renamed/deleted zone since a schedule slot was ---
    // -- saved is exactly the F0 fallback case this feeds into ----------------
    {
        std::vector<LabeledZone> zones; // zone deleted entirely
        String unresolved;
        auto matched = resolveZoneLabels(zones, {String("Kitchen")}, unresolved);
        expectTrue(strEq(unresolved, "Kitchen"), "resolveZoneLabels: no zones at all -> requested label unresolved");
        expectTrue(matched.empty(), "resolveZoneLabels: no zones at all -> no matches");
    }

    if (failures > 0) {
        std::printf("\n%d check(s) FAILED\n", failures);
        return 1;
    }
    std::printf("\nAll checks passed\n");
    return 0;
}
