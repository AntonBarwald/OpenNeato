#ifndef GUIDED_MOTOR_POLICY_H
#define GUIDED_MOTOR_POLICY_H

#include "guided_clean_state.h"

// True while Guided Clean should run the brush/vacuum/side-brush: any state
// actually navigating the route. False while undocking (avoid vacuuming the
// dock), docking/charging (never clean while docking to recharge), or
// idle/done/error.
inline bool guidedMotorsShouldRun(GuidedCleanState s) {
    return s == GUIDED_ROTATING || s == GUIDED_DRIVING || s == GUIDED_ESCAPING || s == GUIDED_CORRECTING;
}

#endif // GUIDED_MOTOR_POLICY_H
