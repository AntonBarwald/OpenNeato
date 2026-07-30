#ifndef SPOT_VERIFY_H
#define SPOT_VERIFY_H

#include <cstring>

// Pure predicate for the sized-spot-clean safety net, extracted so it's host-testable
// without Arduino/NeatoSerial -- same seam pattern as spot_size.h. See spot_verify_selftest.cpp.
//
// True if a spot clean was requested but the robot's polled UI state shows a house clean
// running/paused instead -- the signature of the size-only-Clean-starts-a-house-clean defect.
inline bool isSpotStartMismatch(const char *uiState) {
    if (!uiState)
        return false;
    return std::strstr(uiState, "HOUSECLEANING") != nullptr && std::strstr(uiState, "SPOTCLEANING") == nullptr;
}

#endif // SPOT_VERIFY_H
