#ifndef DRIVE_SPEED_POLICY_H
#define DRIVE_SPEED_POLICY_H

// Pure "scale wheel speed proportionally to step size" (careful-driving spec §4.3, the
// high-confidence smoothing item that needs no protocol-gated assumption about whether a
// second SetMotor overrides one in flight). A slower approach into a hard stop reads as
// controlled, not abrupt, and costs nothing in odometry drift -- drift accumulates with
// distance travelled, not with time or speed. Zero Arduino dependency, host-testable — see
// drive_speed_policy_selftest.cpp.

// magnitude/fullMagnitude share a unit (mm for a drive step, degrees for a rotate step).
// Degenerate fullMagnitude<=0 falls back to maxSpeedMms rather than dividing by zero.
inline int scaledSpeedMms(float magnitude, float fullMagnitude, int minSpeedMms, int maxSpeedMms) {
    if (fullMagnitude <= 0.0f || magnitude >= fullMagnitude)
        return maxSpeedMms;
    if (magnitude <= 0.0f)
        return minSpeedMms;
    float frac = magnitude / fullMagnitude;
    return minSpeedMms + static_cast<int>(static_cast<float>(maxSpeedMms - minSpeedMms) * frac);
}

#endif // DRIVE_SPEED_POLICY_H
