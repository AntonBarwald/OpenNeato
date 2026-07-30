#ifndef NAVIGATION_POC_H
#define NAVIGATION_POC_H

#include <Arduino.h>
#include <vector>
#include "config.h"
#include "loop_task.h"
#include "point_array_json.h"

class NeatoSerial;
class ManualCleanManager;

// #70 — Navigation proof of concept. NOT validated on real hardware.
// Accepts a flat waypoint array (POST /api/navigate) and drives through it one waypoint at
// a time: poll GetRobotPos Raw (Smooth is frozen in TestMode), rotate to face the next waypoint, then drive straight,
// re-polling periodically to correct heading/distance. Deliberately dumb dead-reckoning —
// no path planning, no obstacle avoidance beyond ManualCleanManager's bumper/stall safety.
// Never sends `Clean`/`Clean Stop`/SetEvent — pure TestMode motor control, so there's no
// risk of the "Clean Stop destroys localization" pitfall.
class NavigationPoc : public LoopTask {
public:
    NavigationPoc(NeatoSerial& neato, ManualCleanManager& manual);

    // "t" is ignored — heading is derived from consecutive waypoints, not supplied per-point.
    bool start(const String& bodyJson, String& error);

    // Stop immediately: zero the wheels and exit manual mode. Idempotent.
    void stop();

    bool isActive() const { return active; }

    // GET /api/navigate/status — no serial I/O, reads in-memory state.
    String getStatusJson() const;

private:
    NeatoSerial& neato;
    ManualCleanManager& manualMgr;

    void tick() override;

    enum State {
        POC_IDLE,
        POC_ENABLING, // Waiting for ManualCleanManager::enable(true) to finish
        POC_ROTATING,
        POC_DRIVING,
        POC_DONE,
        POC_ERROR,
    };

    State state = POC_IDLE;
    bool active = false;
    // True only while this instance holds ManualCleanManager (not manualMgr.isActive(),
    // which may reflect another owner) — cleanup paths must gate on this, not isActive().
    bool ownsManualMode = false;
    String lastError;

    std::vector<NavPoint> waypoints;
    size_t waypointIndex = 0;

    Ticker pollTicker;
    bool fetchInFlight = false;
    bool moveInFlight = false; // A manualMgr.move()/enable() call is awaiting its callback

    // Last known pose (meters, degrees) from GetRobotPos Raw
    bool hasPose = false;
    float lastX = 0.0f;
    float lastY = 0.0f;
    float lastTheta = 0.0f;
    unsigned long lastPoseAtMs = 0;

    // Exposed via getStatusJson() to sanity-check the atan2f/theta sign assumption on hardware.
    float lastDistM = 0.0f;
    float lastHeadingErrDeg = 0.0f;

    void pollPose();
    void driveTowardCurrentWaypoint();
    void advanceWaypoint();
    void finish(State endState, const String& error = "");

    // Treats an immediate move() rejection the same as a callback failure — move() returns
    // false WITHOUT calling back when manual mode isn't active, which would otherwise leave
    // moveInFlight stuck true forever.
    void issueMove(int leftMM, int rightMM, int speedMMs, const char *rejectReason);

    static const char *stateName(State s);
};

#endif // NAVIGATION_POC_H
