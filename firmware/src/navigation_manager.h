#ifndef NAVIGATION_MANAGER_H
#define NAVIGATION_MANAGER_H

#include <Arduino.h>
#include <vector>
#include "config.h"
#include "escape_policy.h"
#include "geometry.h"
#include "guided_clean_state.h"
#include "guided_motor_policy.h"
#include "loop_task.h"

class NeatoSerial;
class CleaningHistory;
class ZonesManager;
class ManualCleanManager;
class DataLogger;

// One point of the downsampled waypoint plan built from a recorded session's pose trail.
// Explicit ctor: build here resolves to gnu++11, which needs a real converting ctor for brace-init.
struct GuidedWaypoint {
    Point pt;

    GuidedWaypoint() = default;
    GuidedWaypoint(const Point& pt_) : pt(pt_) {}
};

// #71 — Guided Clean: replays a recorded session's pose trail as a driven route
// instead of re-exploring from scratch. NOT validated on real hardware.
//
//   IDLE --start()--> VALIDATING --> UNDOCKING --> ROTATING <--> DRIVING --> DONE
//                          |            ^  \          |
//                          |            |   \-CORRECTING (reroute)
//                          |
//   Any state --stop()/unrecoverable error/battery low--> IDLE / ERROR
//
// DOCKING never uses `Clean Stop` (destroys D7 localization) — exits manual mode and
// reuses the existing SetEvent-based dock path instead.
// Reuses ManualCleanManager for TestMode/LDS lifecycle and all movement safety; never
// calls NeatoSerial::setMotorWheels() directly.
class NavigationManager : public LoopTask {
public:
    NavigationManager(NeatoSerial& neato, CleaningHistory& history, ZonesManager& zones, ManualCleanManager& manual,
                      DataLogger& logger);

    // POST /api/guided?action=start. A zone label that doesn't resolve fails the whole
    // call rather than silently falling back to the full house.
    bool start(const String& sessionName, const std::vector<String>& zoneLabels, String& error);

    // POST /api/guided?action=stop — abort immediately and return to IDLE. Idempotent.
    void stop();

    bool isActive() const { return state != GUIDED_IDLE && state != GUIDED_DONE && state != GUIDED_ERROR; }

    // GET /api/guided/status — no serial I/O, reads in-memory state.
    String getStatusJson() const;

private:
    NeatoSerial& neato;
    CleaningHistory& history;
    ZonesManager& zonesMgr;
    ManualCleanManager& manualMgr;
    DataLogger& dataLogger;

    void tick() override;

    GuidedCleanState state = GUIDED_IDLE;
    String sessionName;
    String lastError;

    // True only while this instance holds ManualCleanManager (not manualMgr.isActive(),
    // which may reflect another owner) — cleanup paths must gate on this, not isActive().
    bool ownsManualMode = false;

    std::vector<GuidedWaypoint> waypoints;
    size_t waypointIndex = 0;
    int rerouteCount = 0;
    int escapeAttemptsForThisBlock = 0; // Reset on advanceWaypoint(), like rerouteCount

    // Parsed at plan time; segmentIsClear() live-checks every drive-loop segment against
    // these, not just the original consecutive pairs. Multiple zonePolygons = union of
    // matched zones; ignored when zoneFilterActive is false.
    std::vector<std::vector<Point>> noGoLines;
    std::vector<std::vector<Point>> zonePolygons;
    bool zoneFilterActive = false;

    // Last known pose (meters, degrees) from GetRobotPos Raw
    bool hasPose = false;
    float lastX = 0.0f;
    float lastY = 0.0f;
    float lastTheta = 0.0f;
    unsigned long lastPoseAtMs = 0;

    // Odometry route guards — no re-localization, so both are monotonic for the whole route.
    float traveledDistanceM = 0.0f; // Sum of actually-issued drive steps (not straight-line to goal)
    unsigned long routeStartedAtMs = 0; // Set in beginUndock(), backs NAV_MGR_MAX_ODOM_DURATION_MS

    // Set on every beginUndock() completion; consumed by the next pollPose() to skip leading
    // waypoints the undock move already covered (see nav_geometry.h's navGeoSkipCoveredWaypoints).
    bool waypointsSkipPending = false;
    Point recordingFirstPoint{}; // recording's own first pose, before zone filtering

    bool routeTruncated = false; // set at plan time, persists for the life of the run
    float plannedPathM = 0.0f; // pre-truncation length (the "would have been" figure)

    Ticker pollTicker;
    Ticker batteryTicker;
    bool fetchInFlight = false;
    bool waitingOnCallback = false; // A move()/enable()/clean() call is in flight


    // -- Planning (run once from start()) -------------------------------------
    bool loadWaypoints(const String& forSession, String& error);
    // Resolves zoneLabels against the session's saved zones; false with error set if a
    // label doesn't resolve.
    bool applyZoneFilterAndNoGo(const String& forSession, const std::vector<String>& zoneLabels, String& error);
    size_t findNearestWaypoint(float x, float y) const; // Returns waypointIndex if no alternative found

    // True if from->to crosses no no-go line and (if zone filter active) stays inside the
    // zone polygon; re-evaluated live each call, not just at plan time.
    bool segmentIsClear(const Point& from, const Point& to) const;

    // -- Drive loop -------------------------------------------------------------
    void pollPose();
    void driveTowardCurrentWaypoint();
    // LIDAR proximity veto (careful-driving spec §2) — scans while still stationary, before
    // the drive step is issued; shortens/stops the step or defers to handleBlocked().
    void checkLidarThenDrive(int requestedMm);
    void advanceWaypoint();
    // On a physical block, tries the escape maneuver (careful-driving spec §3) before falling
    // through to rerouteAfterBlock(); typedReason defaults to "no typed reason, geometric" for
    // call sites with nothing physical to escape from (no-go/zone crossing, LIDAR pre-veto).
    void handleBlocked(const char *reason, MoveBlockReason typedReason = MoveBlockReason::kNone);
    // The pre-existing pure-geometric reroute/fail logic — findNearestWaypoint() + budget check.
    void rerouteAfterBlock(const char *reason);

    // -- Escape maneuver (spec §3): reverse (or forward, kStallRear mirror) then turn-away.
    // Async chain: beginEscape() -> [issueEscapeTurn()] -> finishEscape() -> rerouteAfterBlock().
    void beginEscape(const char *reason, const EscapeMotion& escape);
    void issueEscapeTurn(const char *reason, EscapeTurnDir dir, int rotateDeg);
    void finishEscape(const char *reason, bool turnCompleted);

    // Treats an immediate (synchronous) move() rejection the same as a callback failure —
    // move()'s "!active" path never invokes its callback.
    void issueMove(int leftMM, int rightMM, int speedMMs, const char *rejectReason);

    // -- Undock / dock / charge transitions ------------------------------------
    void beginUndock();
    void finishRoute();
    void checkBattery();

    void setState(GuidedCleanState s);
    void applyMotorPolicy(GuidedCleanState forState);
    void fail(const String& reason);

    static const char *stateName(GuidedCleanState s);
};

#endif // NAVIGATION_MANAGER_H
