#ifndef LDS_SECTOR_H
#define LDS_SECTOR_H

// Pure LIDAR-sector reduction backing the proactive proximity veto (careful-driving spec
// §2). Zero Arduino dependency, host-testable — see lds_sector_selftest.cpp. Coarse
// is-anything-closer-than-X check, not body-edge clearance (turret offset is unreliable —
// see manual_clean_manager.h).

constexpr int LDS_NUM_POINTS = 360; // GetLDSScan returns points[0..359], angleDeg == index

// Reduction of one sector of a GetLDSScan reading to a single trust-worthy minimum distance.
// reliable=false means too few valid (non-error, non-zero) samples to trust minDistMm at all.
// Explicit ctor: gnu++11 (pio check) needs a real converting ctor for brace-init on a struct
// with default member initializers -- same precedent as motion_safety.h's MotionSafetyFlags.
struct LdsSectorResult {
    int minDistMm = -1;
    int validSampleCount = 0;
    bool reliable = false;

    LdsSectorResult() = default;
    LdsSectorResult(int minDistMm_, int validSampleCount_, bool reliable_) :
        minDistMm(minDistMm_), validSampleCount(validSampleCount_), reliable(reliable_) {}
};

// offsetDeg: bench-measured heading-zero offset (NAV_LDS_FRONT_OFFSET_DEG).
// sectorCenterDeg/sectorHalfWidthDeg: robot-relative sector, e.g. 0/20 for straight ahead.
// distMm[i]==0 ("no reading") and errorCode[i]!=0 samples are excluded, not folded into the min.
inline LdsSectorResult ldsSectorMinDistance(const int distMm[LDS_NUM_POINTS], const int errorCode[LDS_NUM_POINTS],
                                            int offsetDeg, int sectorCenterDeg, int sectorHalfWidthDeg,
                                            int minValidSamples) {
    LdsSectorResult result;
    int minDist = -1;
    int count = 0;
    for (int delta = -sectorHalfWidthDeg; delta <= sectorHalfWidthDeg; delta++) {
        int idx = ((offsetDeg + sectorCenterDeg + delta) % 360 + 360) % 360;
        if (errorCode[idx] != 0)
            continue;
        if (distMm[idx] == 0)
            continue;
        count++;
        if (minDist < 0 || distMm[idx] < minDist)
            minDist = distMm[idx];
    }
    result.validSampleCount = count;
    result.reliable = count >= minValidSamples;
    result.minDistMm = minDist;
    return result;
}

enum class LdsDriveAction { Clear, Shorten, Stop };

// Explicit ctor: same gnu++11 aggregate-init caveat as LdsSectorResult above.
struct LdsDriveDecision {
    LdsDriveAction action = LdsDriveAction::Clear;
    int adjustedStepMm = 0; // Clear: requestedStepMm; Shorten: the shortened step; Stop: 0

    LdsDriveDecision() = default;
    LdsDriveDecision(LdsDriveAction action_, int adjustedStepMm_) : action(action_), adjustedStepMm(adjustedStepMm_) {}
};

// Careful-driving spec §2.4's threshold table. An unreliable sector falls back to Clear
// (proceed at the requested step) -- bumper/stall/drop stay the backstop either way.
inline LdsDriveDecision ldsDriveDecision(const LdsSectorResult& sector, int requestedStepMm, int footprintRadiusMm,
                                         int stopMarginMm, int minUsefulStepMm) {
    if (!sector.reliable)
        return LdsDriveDecision{LdsDriveAction::Clear, requestedStepMm};

    int stopThreshold = footprintRadiusMm + stopMarginMm;
    if (sector.minDistMm <= stopThreshold)
        return LdsDriveDecision{LdsDriveAction::Stop, 0};

    int clearThreshold = requestedStepMm + footprintRadiusMm + stopMarginMm;
    if (sector.minDistMm > clearThreshold)
        return LdsDriveDecision{LdsDriveAction::Clear, requestedStepMm};

    int shortened = sector.minDistMm - footprintRadiusMm - stopMarginMm;
    if (shortened < minUsefulStepMm)
        return LdsDriveDecision{LdsDriveAction::Stop, 0};
    return LdsDriveDecision{LdsDriveAction::Shorten, shortened};
}

#endif // LDS_SECTOR_H
