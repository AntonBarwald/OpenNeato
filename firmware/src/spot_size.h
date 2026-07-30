#ifndef SPOT_SIZE_H
#define SPOT_SIZE_H

// Pure clamp logic for sized spot clean's Width/Height args, extracted so it's host-testable
// without Arduino/NeatoSerial — same seam pattern as motion_safety.h. See spot_size_selftest.cpp.

// raw <= 0 (HTTP param absent/0, or the -1 sentinel) -> -1 (no override, robot default).
// raw in [1, 99] -> clamp up to the protocol floor, 100.
// raw in [100, 400] -> pass through.
// raw > 400 -> clamp down to the protocol ceiling, 400.
inline int clampSpotDimension(int raw) {
    if (raw <= 0)
        return -1;
    if (raw < 100)
        return 100;
    if (raw > 400)
        return 400;
    return raw;
}

#endif // SPOT_SIZE_H
