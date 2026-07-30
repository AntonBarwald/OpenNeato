#include "navigation_poc.h"
#include <cmath>
#include "json_fields.h"
#include "manual_clean_manager.h"
#include "neato_serial.h"

namespace {
    constexpr float kRadToDeg = 57.29577951308232f;

    // Wrap a degree delta into (-180, 180].
    float normalizeDeg(float deg) {
        while (deg > 180.0f)
            deg -= 360.0f;
        while (deg <= -180.0f)
            deg += 360.0f;
        return deg;
    }
} // namespace

NavigationPoc::NavigationPoc(NeatoSerial& neato, ManualCleanManager& manual) :
    LoopTask(0), neato(neato), manualMgr(manual) {
    TaskRegistry::add(this);
}

const char *NavigationPoc::stateName(State s) {
    switch (s) {
        case POC_IDLE:
            return "idle";
        case POC_ENABLING:
            return "enabling";
        case POC_ROTATING:
            return "rotating";
        case POC_DRIVING:
            return "driving";
        case POC_DONE:
            return "done";
        case POC_ERROR:
            return "error";
    }
    return "unknown";
}

// -- Lifecycle ----------------------------------------------------------------

bool NavigationPoc::start(const String& bodyJson, String& error) {
    error = "";

    if (active) {
        error = "navigation already in progress";
        return false;
    }

    std::vector<NavPoint> parsed;
    if (!parsePointArray(bodyJson, parsed)) {
        error = "invalid waypoint array";
        return false;
    }
    if (parsed.empty()) {
        error = "waypoint array is empty";
        return false;
    }
    if (parsed.size() > NAV_MAX_WAYPOINTS) {
        error = "too many waypoints (max " + String(NAV_MAX_WAYPOINTS) + ")";
        return false;
    }

    waypoints = std::move(parsed);
    waypointIndex = 0;
    hasPose = false;
    lastError = "";
    lastDistM = 0.0f;
    lastHeadingErrDeg = 0.0f;
    active = true;
    state = POC_ENABLING;
    moveInFlight = true;

    LOG("NAVPOC", "Starting navigation POC: %u waypoints", static_cast<unsigned>(waypoints.size()));

    bool accepted = manualMgr.enable(true, [this](bool ok) {
        moveInFlight = false;
        // stop() may have run while acquiring — release and bail
        if (!active) {
            if (ok)
                manualMgr.enable(false, nullptr);
            LOG("NAVPOC", "Manual mode acquisition resolved after stop — discarding");
            return;
        }
        if (!ok) {
            finish(POC_ERROR, "failed to enter manual mode");
            return;
        }
        ownsManualMode = true;
        manualMgr.setClientWatchdogSuppressed(true);
        state = POC_ROTATING;
        pollTicker.reset(); // Poll immediately instead of waiting a full interval
    });
    // enable() invokes its callback synchronously on every rejection path
    // (already active/enabling), so `accepted` is only a defensive check —
    // unlike move(), there's no callback-never-fires path here.
    if (!accepted) {
        moveInFlight = false;
    }

    return true;
}

void NavigationPoc::stop() {
    if (!active)
        return;
    LOG("NAVPOC", "Stopped by request (waypoint %u/%u)", static_cast<unsigned>(waypointIndex),
        static_cast<unsigned>(waypoints.size()));
    if (ownsManualMode) {
        manualMgr.move(0, 0, 0, nullptr);
        manualMgr.enable(false, nullptr);
        manualMgr.setClientWatchdogSuppressed(false);
        ownsManualMode = false;
    }
    active = false;
    moveInFlight = false;
    state = POC_IDLE;
}

void NavigationPoc::finish(State endState, const String& error) {
    lastError = error;
    if (ownsManualMode) {
        manualMgr.move(0, 0, 0, nullptr);
        manualMgr.enable(false, nullptr);
        manualMgr.setClientWatchdogSuppressed(false);
        ownsManualMode = false;
    }
    active = false;
    moveInFlight = false;
    state = endState;
    if (!error.isEmpty())
        LOG("NAVPOC", "Finished with error: %s", error.c_str());
    else
        LOG("NAVPOC", "Finished: all waypoints reached");
}

// -- Main loop ------------------------------------------------------------------

void NavigationPoc::tick() {
    if (!active)
        return;
    if (state == POC_ENABLING || moveInFlight)
        return; // Waiting on a callback — don't overlap commands

    // Stale-pose watchdog — if we've never seen a pose, or the last good fix
    // is too old, bail out rather than continuing to drive blind.
    if (hasPose && millis() - lastPoseAtMs > NAV_STALE_POSE_MS) {
        finish(POC_ERROR, "no position fix for too long");
        return;
    }

    if (pollTicker.elapsed(NAV_POLL_MS)) {
        pollPose();
    }
}

void NavigationPoc::pollPose() {
    if (fetchInFlight)
        return;
    fetchInFlight = true;
    neato.getRobotPos(false, [this](bool ok, const RobotPosData& pos) {
        fetchInFlight = false;
        if (!active)
            return;
        if (!ok || !pos.hasPose) {
            LOG("NAVPOC", "GetRobotPos fetch/parse failed, will retry next poll");
            return;
        }
        lastX = pos.x;
        lastY = pos.y;
        lastTheta = pos.theta;
        hasPose = true;
        lastPoseAtMs = millis();
        driveTowardCurrentWaypoint();
    });
}

void NavigationPoc::issueMove(int leftMM, int rightMM, int speedMMs, const char *rejectReason) {
    moveInFlight = true;
    bool accepted = manualMgr.move(leftMM, rightMM, speedMMs, [this](bool ok) {
        moveInFlight = false;
        if (!ok)
            finish(POC_ERROR, moveBlockReasonToString(manualMgr.lastBlockReason()));
    });
    if (!accepted) {
        // move() returned false WITHOUT invoking the callback (manual mode
        // isn't active) — resolve synchronously instead of waiting forever.
        moveInFlight = false;
        finish(POC_ERROR, rejectReason);
    }
}

void NavigationPoc::driveTowardCurrentWaypoint() {
    if (waypointIndex >= waypoints.size()) {
        finish(POC_DONE);
        return;
    }

    const Point& target = waypoints[waypointIndex].pt;
    float dx = target.x - lastX;
    float dy = target.y - lastY;
    float dist = sqrtf(dx * dx + dy * dy);
    lastDistM = dist;

    if (dist <= NAV_ARRIVAL_THRESHOLD_M) {
        advanceWaypoint();
        return;
    }

    float targetHeadingDeg = atan2f(dy, dx) * kRadToDeg;
    float headingErr = normalizeDeg(targetHeadingDeg - lastTheta);
    lastHeadingErrDeg = headingErr;

    if (fabsf(headingErr) > NAV_HEADING_TOLERANCE_DEG) {
        state = POC_ROTATING;
        float step = headingErr;
        if (step > NAV_MAX_ROTATE_STEP_DEG)
            step = NAV_MAX_ROTATE_STEP_DEG;
        if (step < -NAV_MAX_ROTATE_STEP_DEG)
            step = -NAV_MAX_ROTATE_STEP_DEG;

        // Arc length each wheel travels (opposite directions) to rotate the
        // chassis by `step` degrees in place, given the wheel track width.
        float arcLenMm = NAV_TRACK_WIDTH_MM * static_cast<float>(PI) * (fabsf(step) / 360.0f);
        int mag = static_cast<int>(roundf(arcLenMm));
        int sign = (step >= 0.0f) ? 1 : -1;
        sign *= NAV_ROTATE_SIGN;
        int leftMM = -sign * mag;
        int rightMM = sign * mag;

        issueMove(leftMM, rightMM, NAV_ROTATE_SPEED_MMS, "rotate move rejected (manual mode inactive)");
    } else {
        state = POC_DRIVING;
        float step = fminf(dist, NAV_MAX_DRIVE_STEP_M);
        int mm = static_cast<int>(roundf(step * 1000.0f));

        issueMove(mm, mm, NAV_DRIVE_SPEED_MMS, "drive move rejected (manual mode inactive)");
    }
}

void NavigationPoc::advanceWaypoint() {
    waypointIndex++;
    LOG("NAVPOC", "Waypoint %u/%u reached", static_cast<unsigned>(waypointIndex),
        static_cast<unsigned>(waypoints.size()));
    if (waypointIndex >= waypoints.size()) {
        finish(POC_DONE);
    }
    // Otherwise: next poll tick evaluates the new current waypoint.
}

// -- Status ---------------------------------------------------------------------

String NavigationPoc::getStatusJson() const {
    return fieldsToJson({
            {"active", active ? "true" : "false", FIELD_BOOL},
            {"state", stateName(state), FIELD_STRING},
            {"waypointIndex", String(static_cast<unsigned>(waypointIndex)), FIELD_INT},
            {"waypointCount", String(static_cast<unsigned>(waypoints.size())), FIELD_INT},
            {"hasPose", hasPose ? "true" : "false", FIELD_BOOL},
            {"lastX", String(lastX, 3), FIELD_FLOAT},
            {"lastY", String(lastY, 3), FIELD_FLOAT},
            {"lastTheta", String(lastTheta, 1), FIELD_FLOAT},
            {"lastDistM", String(lastDistM, 3), FIELD_FLOAT},
            {"lastHeadingErrDeg", String(lastHeadingErrDeg, 1), FIELD_FLOAT},
            {"error", lastError, FIELD_STRING},
    });
}
