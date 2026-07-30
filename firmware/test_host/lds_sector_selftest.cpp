// Host-only self-test for firmware/src/lds_sector.h — written BEFORE that header exists, per
// the TDD requirement for docs/spec-careful-driving.md §2/§6. Covers the pure sector-reduction
// (ldsSectorMinDistance) and drive-decision (ldsDriveDecision) functions backing the LIDAR
// proximity veto. This file does not compile until lds_sector.h is written (the actual RED).
//
// Run:
//   clang++ -std=c++17 -I firmware/src firmware/test_host/lds_sector_selftest.cpp -o /tmp/ldstest && /tmp/ldstest

#include "lds_sector.h"
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

namespace {
    void fillClear(int distMm[LDS_NUM_POINTS], int errorCode[LDS_NUM_POINTS], int dist = 2000) {
        for (int i = 0; i < LDS_NUM_POINTS; i++) {
            distMm[i] = dist;
            errorCode[i] = 0;
        }
    }
} // namespace

int main() {
    // -- ldsSectorMinDistance ---------------------------------------------------------

    // All-clear sector: min distance is the uniform value, sample count is the full sector width.
    {
        int dist[LDS_NUM_POINTS], err[LDS_NUM_POINTS];
        fillClear(dist, err, 2000);
        LdsSectorResult r = ldsSectorMinDistance(dist, err, /*offsetDeg=*/0, /*sectorCenterDeg=*/0,
                                                  /*sectorHalfWidthDeg=*/20, /*minValidSamples=*/5);
        expectTrue(r.reliable, "all-clear: reliable");
        expectTrue(r.minDistMm == 2000, "all-clear: minDistMm is the uniform distance");
        expectTrue(r.validSampleCount == 41, "all-clear: 41 samples in a +/-20deg sector (2*20+1)");
    }

    // A single close reading inside the sector pulls the minimum down.
    {
        int dist[LDS_NUM_POINTS], err[LDS_NUM_POINTS];
        fillClear(dist, err, 2000);
        dist[5] = 300; // within the default 0-centered +/-20 sector
        LdsSectorResult r = ldsSectorMinDistance(dist, err, 0, 0, 20, 5);
        expectTrue(r.minDistMm == 300, "close reading inside sector pulls min down");
    }

    // A close reading OUTSIDE the sector must not affect the result.
    {
        int dist[LDS_NUM_POINTS], err[LDS_NUM_POINTS];
        fillClear(dist, err, 2000);
        dist[90] = 10; // far outside a +/-20deg sector centered at 0
        LdsSectorResult r = ldsSectorMinDistance(dist, err, 0, 0, 20, 5);
        expectTrue(r.minDistMm == 2000, "close reading outside sector is ignored");
    }

    // Sector wrap-around at the 0/359 boundary: sector centered at 0 spans ..340..359,0..20..
    {
        int dist[LDS_NUM_POINTS], err[LDS_NUM_POINTS];
        fillClear(dist, err, 2000);
        dist[350] = 400; // -10deg relative to 0, i.e. index 350 wrapping from 359
        LdsSectorResult r = ldsSectorMinDistance(dist, err, 0, 0, 20, 5);
        expectTrue(r.minDistMm == 400, "wrap-around: a reading just past the 359/0 boundary counts");
    }

    // errorCode != 0 samples are excluded from the min-distance computation and the valid count.
    {
        int dist[LDS_NUM_POINTS], err[LDS_NUM_POINTS];
        fillClear(dist, err, 2000);
        dist[3] = 50; // would otherwise be the minimum
        err[3] = 1; // errored -- must be excluded
        LdsSectorResult r = ldsSectorMinDistance(dist, err, 0, 0, 20, 5);
        expectTrue(r.minDistMm == 2000, "errored sample excluded from min-distance");
        expectTrue(r.validSampleCount == 40, "errored sample excluded from valid count (41 - 1)");
    }

    // distMM == 0 ("no reading") is treated as unknown, not clear -- excluded like an error.
    {
        int dist[LDS_NUM_POINTS], err[LDS_NUM_POINTS];
        fillClear(dist, err, 2000);
        dist[3] = 0;
        LdsSectorResult r = ldsSectorMinDistance(dist, err, 0, 0, 20, 5);
        expectTrue(r.minDistMm == 2000, "zero-distance sample excluded from min-distance");
        expectTrue(r.validSampleCount == 40, "zero-distance sample excluded from valid count");
    }

    // All-error sector: unreliable (below minValidSamples), regardless of what minDistMm holds.
    {
        int dist[LDS_NUM_POINTS], err[LDS_NUM_POINTS];
        fillClear(dist, err, 2000);
        for (int i = 340; i < 360; i++)
            err[i] = 1;
        for (int i = 0; i <= 20; i++)
            err[i] = 1;
        LdsSectorResult r = ldsSectorMinDistance(dist, err, 0, 0, 20, 5);
        expectTrue(!r.reliable, "all-error sector: unreliable");
        expectTrue(r.validSampleCount == 0, "all-error sector: zero valid samples");
    }

    // All-zero-distance sector: same as all-error -- unreliable.
    {
        int dist[LDS_NUM_POINTS], err[LDS_NUM_POINTS];
        fillClear(dist, err, 2000);
        for (int i = 340; i < 360; i++)
            dist[i] = 0;
        for (int i = 0; i <= 20; i++)
            dist[i] = 0;
        LdsSectorResult r = ldsSectorMinDistance(dist, err, 0, 0, 20, 5);
        expectTrue(!r.reliable, "all-zero-distance sector: unreliable");
    }

    // Mixed valid/invalid: exactly at the minValidSamples threshold is reliable.
    {
        int dist[LDS_NUM_POINTS], err[LDS_NUM_POINTS];
        fillClear(dist, err, 2000);
        // Sector has 41 samples; error all but exactly 5 of them.
        for (int i = -20; i <= 20; i++) {
            int idx = ((i % 360) + 360) % 360;
            err[idx] = 1;
        }
        err[0] = 0;
        err[1] = 0;
        err[2] = 0;
        err[3] = 0;
        err[4] = 0;
        LdsSectorResult r = ldsSectorMinDistance(dist, err, 0, 0, 20, 5);
        expectTrue(r.reliable, "exactly minValidSamples valid readings: reliable");
        expectTrue(r.validSampleCount == 5, "exactly minValidSamples valid readings: count is 5");
    }

    // One below the threshold: unreliable.
    {
        int dist[LDS_NUM_POINTS], err[LDS_NUM_POINTS];
        fillClear(dist, err, 2000);
        for (int i = -20; i <= 20; i++) {
            int idx = ((i % 360) + 360) % 360;
            err[idx] = 1;
        }
        err[0] = 0;
        err[1] = 0;
        err[2] = 0;
        err[3] = 0;
        LdsSectorResult r = ldsSectorMinDistance(dist, err, 0, 0, 20, 5);
        expectTrue(!r.reliable, "one below minValidSamples: unreliable");
    }

    // NAV_LDS_FRONT_OFFSET_DEG (offsetDeg) shifts which raw indices count as "ahead".
    {
        int dist[LDS_NUM_POINTS], err[LDS_NUM_POINTS];
        fillClear(dist, err, 2000);
        dist[100] = 250; // would be "ahead" if offsetDeg == 100
        LdsSectorResult r0 = ldsSectorMinDistance(dist, err, /*offsetDeg=*/0, 0, 20, 5);
        expectTrue(r0.minDistMm == 2000, "offsetDeg=0: index 100 is not in the forward sector");
        LdsSectorResult r100 = ldsSectorMinDistance(dist, err, /*offsetDeg=*/100, 0, 20, 5);
        expectTrue(r100.minDistMm == 250, "offsetDeg=100: index 100 is now the forward-relative center");
    }

    // -- ldsDriveDecision --------------------------------------------------------------

    // Clear: min distance comfortably beyond requestedStep + footprint + margin -> full step.
    {
        LdsSectorResult sector{2000, 41, true};
        LdsDriveDecision d = ldsDriveDecision(sector, /*requestedStepMm=*/500, /*footprintRadiusMm=*/200,
                                              /*stopMarginMm=*/80, /*minUsefulStepMm=*/50);
        expectTrue(d.action == LdsDriveAction::Clear, "clear zone: full step issued");
        expectTrue(d.adjustedStepMm == 500, "clear zone: adjustedStepMm equals requested step");
    }

    // Stop: min distance at/under footprint+margin -> no move at all.
    {
        LdsSectorResult sector{250, 41, true}; // <= 200+80
        LdsDriveDecision d = ldsDriveDecision(sector, 500, 200, 80, 50);
        expectTrue(d.action == LdsDriveAction::Stop, "stop zone: at/under footprint+margin stops");
        expectTrue(d.adjustedStepMm == 0, "stop zone: adjustedStepMm is 0");
    }

    // Shorten: between the two thresholds -> shortened step, proportional to available room.
    {
        LdsSectorResult sector{600, 41, true}; // > 280 (stop) but < 500+280 (clear)
        LdsDriveDecision d = ldsDriveDecision(sector, 500, 200, 80, 50);
        expectTrue(d.action == LdsDriveAction::Shorten, "shorten zone: between stop and clear thresholds");
        expectTrue(d.adjustedStepMm == 600 - 200 - 80, "shorten zone: adjustedStepMm is minDist - footprint - margin");
    }

    // Shorten collapsing below minUsefulStepMm is treated as Stop instead of a near-zero move.
    {
        LdsSectorResult sector{300, 41, true}; // 300-200-80 = 20mm, below minUsefulStepMm=50
        LdsDriveDecision d = ldsDriveDecision(sector, 500, 200, 80, 50);
        expectTrue(d.action == LdsDriveAction::Stop, "shortened-below-minimum collapses to Stop");
    }

    // Unreliable sector: fall back to proceeding at the requested step (bumper/stall backstop).
    {
        LdsSectorResult sector{}; // reliable=false by default
        LdsDriveDecision d = ldsDriveDecision(sector, 500, 200, 80, 50);
        expectTrue(d.action == LdsDriveAction::Clear, "unreliable sector: falls back to Clear (bumper/stall backstop)");
        expectTrue(d.adjustedStepMm == 500, "unreliable sector: full requested step issued");
    }

    if (failures > 0) {
        std::printf("\n%d check(s) FAILED\n", failures);
        return 1;
    }
    std::printf("\nAll checks passed\n");
    return 0;
}
