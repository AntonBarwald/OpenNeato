#include "navigation_manager.h"
#include <cctype>
#include <cmath>
#include "cleaning_history.h"
#include "data_logger.h"
#include "drive_speed_policy.h"
#include "json_fields.h"
#include "lds_sector.h"
#include "manual_clean_manager.h"
#include "nav_geometry.h"
#include "neato_serial.h"
#include "point_array_json.h"
#include "zone_label_match.h"
#include "zones_manager.h"

namespace {

    // -- Ad-hoc parser for the frontend's opaque zones blob (ZonesManager stores it verbatim
    // and never parses it) ----------------------------------------------------------------
    // Deliberately permissive: malformed input stops early rather than erroring, so an
    // unparseable zones blob degrades to "no filtering" instead of blocking guided clean.

    int findMatchingBracket(const String& s, int openPos) {
        int len = static_cast<int>(s.length());
        if (openPos < 0 || openPos >= len)
            return -1;
        char open = s.charAt(openPos);
        if (open != '{' && open != '[')
            return -1;
        char close = (open == '{') ? '}' : ']';
        int depth = 0;
        bool inString = false;
        for (int i = openPos; i < len; i++) {
            char c = s.charAt(i);
            if (inString) {
                if (c == '\\') {
                    i++; // Skip the escaped character
                    continue;
                }
                if (c == '"')
                    inString = false;
                continue;
            }
            if (c == '"') {
                inString = true;
                continue;
            }
            if (c == open) {
                depth++;
            } else if (c == close) {
                depth--;
                if (depth == 0)
                    return i;
            }
        }
        return -1;
    }

    // Locates the array value of `"key":[...]`; sets both to -1 if not found.
    void findArrayField(const String& s, const char *key, int& openPos, int& closePos) {
        openPos = -1;
        closePos = -1;
        String pattern = String("\"") + key + "\"";
        int keyPos = s.indexOf(pattern);
        if (keyPos < 0)
            return;
        int colon = s.indexOf(':', keyPos + pattern.length());
        if (colon < 0)
            return;
        int i = colon + 1;
        int len = static_cast<int>(s.length());
        while (i < len && isspace(static_cast<unsigned char>(s.charAt(i))))
            i++;
        if (i >= len || s.charAt(i) != '[')
            return;
        openPos = i;
        closePos = findMatchingBracket(s, openPos);
    }

    // Walks a top-level JSON array of objects, invoking `perObject` per `{...}` element.
    template<typename Fn>
    void forEachObjectIn(const String& s, int openPos, int closePos, Fn perObject) {
        if (openPos < 0 || closePos < 0 || closePos <= openPos)
            return;
        int i = openPos + 1;
        while (i < closePos) {
            while (i < closePos && (isspace(static_cast<unsigned char>(s.charAt(i))) || s.charAt(i) == ','))
                i++;
            if (i >= closePos || s.charAt(i) != '{')
                break; // Malformed — stop rather than misparse
            int objEnd = findMatchingBracket(s, i);
            if (objEnd < 0 || objEnd > closePos)
                break;
            perObject(s.substring(i, objEnd + 1));
            i = objEnd + 1;
        }
    }

    // Extracts a zone/no-go-line object's "points" array via point_array_json's parser.
    void parsePolyline(const String& obj, std::vector<Point>& out) {
        out.clear();
        int openPos, closePos;
        findArrayField(obj, "points", openPos, closePos);
        if (openPos < 0 || closePos < 0)
            return;
        std::vector<NavPoint> pts;
        if (!parsePointArray(obj.substring(openPos, closePos + 1), pts))
            return;
        out.reserve(pts.size());
        for (const auto& p: pts)
            out.push_back(p.pt);
    }

    // Extracts a top-level string field (e.g. "label"); json_fields.h's fieldsFromJson
    // can't be reused since it bails on the object's nested "points" array.
    bool extractStringField(const String& obj, const char *key, String& out) {
        String pattern = String("\"") + key + "\"";
        int keyPos = obj.indexOf(pattern.c_str());
        if (keyPos < 0)
            return false;
        int colon = obj.indexOf(':', keyPos + pattern.length());
        if (colon < 0)
            return false;
        int i = colon + 1;
        int len = static_cast<int>(obj.length());
        while (i < len && isspace(static_cast<unsigned char>(obj.charAt(i))))
            i++;
        if (i >= len || obj.charAt(i) != '"')
            return false;
        i++;
        String value;
        while (i < len) {
            char c = obj.charAt(i);
            if (c == '\\' && i + 1 < len) {
                i++;
                value += obj.charAt(i);
                i++;
                continue;
            }
            if (c == '"') {
                out = value;
                return true;
            }
            value += c;
            i++;
        }
        return false;
    }

    void parseZonesBlob(const String& blob, std::vector<LabeledZone>& zones,
                        std::vector<std::vector<Point>>& noGoLines) {
        zones.clear();
        noGoLines.clear();
        if (blob.isEmpty())
            return;

        int zOpen, zClose;
        findArrayField(blob, "zones", zOpen, zClose);
        int idx = 0;
        forEachObjectIn(blob, zOpen, zClose, [&](const String& obj) {
            std::vector<Point> pts;
            parsePolyline(obj, pts);
            if (!pts.empty()) {
                String explicitLabel;
                extractStringField(obj, "label", explicitLabel); // leaves explicitLabel empty if absent
                zones.push_back({zoneLabelFor(explicitLabel, idx), std::move(pts)});
            }
            idx++;
        });

        int nOpen, nClose;
        findArrayField(blob, "noGoLines", nOpen, nClose);
        forEachObjectIn(blob, nOpen, nClose, [&](const String& obj) {
            std::vector<Point> pts;
            parsePolyline(obj, pts);
            if (!pts.empty())
                noGoLines.push_back(std::move(pts));
        });
    }

    constexpr float kRadToDeg = 57.29577951308232f;

    float normalizeDeg(float deg) {
        while (deg > 180.0f)
            deg -= 360.0f;
        while (deg <= -180.0f)
            deg += 360.0f;
        return deg;
    }

} // namespace

NavigationManager::NavigationManager(NeatoSerial& neato, CleaningHistory& history, ZonesManager& zones,
                                     ManualCleanManager& manual, DataLogger& logger) :
    LoopTask(0), neato(neato), history(history), zonesMgr(zones), manualMgr(manual), dataLogger(logger) {
    TaskRegistry::add(this);
}

const char *NavigationManager::stateName(GuidedCleanState s) {
    switch (s) {
        case GUIDED_IDLE:
            return "idle";
        case GUIDED_VALIDATING:
            return "validating";
        case GUIDED_UNDOCKING:
            return "undocking";
        case GUIDED_ROTATING:
            return "rotating";
        case GUIDED_DRIVING:
            return "driving";
        case GUIDED_ESCAPING:
            return "escaping";
        case GUIDED_CORRECTING:
            return "correcting";
        case GUIDED_DOCKING:
            return "docking";
        case GUIDED_CHARGING:
            return "charging";
        case GUIDED_DONE:
            return "done";
        case GUIDED_ERROR:
            return "error";
    }
    return "unknown";
}

// -- Planning (run synchronously from start()) --------------------------------

bool NavigationManager::loadWaypoints(const String& forSession, String& error) {
    waypoints.clear();

    auto reader = history.readSession(forSession);
    if (!reader) {
        error = "session not found";
        return false;
    }

    // Collect the whole raw pose trail first, then downsample in one pass (see
    // navGeoDownsampleTrail) — scanning must never stop early, or any zone whose geometry
    // lives past NAV_MGR_MAX_WAYPOINTS raw poses silently resolves to zero waypoints.
    std::vector<Point> rawTrail;

    auto handleLine = [&](String line) {
        line.trim();
        if (line.isEmpty())
            return;

        auto fields = fieldsFromJson(line);
        if (fields.empty())
            return;

        // Only bare pose snapshots (no "type" field) are waypoint candidates.
        if (findField(fields, "type"))
            return;

        const Field *xf = findField(fields, "x");
        const Field *yf = findField(fields, "y");
        if (!xf || !yf)
            return;

        rawTrail.push_back({xf->value.toFloat(), yf->value.toFloat()});
    };

    uint8_t buf[256];
    size_t n;
    String current;
    while ((n = reader->read(buf, sizeof(buf))) > 0) {
        for (size_t i = 0; i < n; i++) {
            char c = static_cast<char>(buf[i]);
            if (c == '\n') {
                handleLine(current);
                current = "";
            } else {
                current += c;
            }
        }
    }
    if (!current.isEmpty())
        handleLine(current);

    if (rawTrail.empty()) {
        error = "session pose trail is empty";
        return false;
    }

    recordingFirstPoint = rawTrail.front();
    std::vector<Point> downsampled =
            navGeoDownsampleTrail(rawTrail, NAV_MGR_DOWNSAMPLE_MIN_DIST_M, NAV_MGR_MAX_WAYPOINTS);
    waypoints.reserve(downsampled.size());
    for (const auto& p: downsampled)
        waypoints.push_back({p});

    return true;
}

bool NavigationManager::applyZoneFilterAndNoGo(const String& forSession, const std::vector<String>& zoneLabels,
                                               String& error) {
    String blob = zonesMgr.getZones(forSession);
    std::vector<LabeledZone> zones;
    std::vector<std::vector<Point>> noGoLinesParsed;
    if (!blob.isEmpty())
        parseZonesBlob(blob, zones, noGoLinesParsed);

    // Retained regardless of zone filtering — segmentIsClear() live-checks against this
    // on every drive-loop call, not just once here at plan time.
    this->noGoLines = std::move(noGoLinesParsed);

    if (zoneLabels.empty()) {
        zoneFilterActive = false;
        zonePolygons.clear();
        return true; // No zone restriction requested — keep the full path
    }

    // Resolve labels against the session's saved zones (union, not intersection); a label
    // that doesn't resolve fails the whole call rather than silently using the full house.
    String unresolved;
    zonePolygons = resolveZoneLabels(zones, zoneLabels, unresolved);
    if (!unresolved.isEmpty()) {
        error = "zone not found: " + unresolved;
        zonePolygons.clear();
        return false;
    }
    zoneFilterActive = true;

    std::vector<GuidedWaypoint> filtered;
    filtered.reserve(waypoints.size());
    for (const auto& wp: waypoints) {
        for (const auto& poly: zonePolygons) {
            if (pointInPolygon(wp.pt, poly)) {
                filtered.push_back(wp);
                break;
            }
        }
    }
    waypoints = std::move(filtered);
    return true;
}

// Thin wrapper binding nav_geometry.h's host-testable navGeoSegmentIsClearZones() to this
// instance's live-retained noGoLines/zonePolygons; evaluated fresh against the robot's
// current position every call, not just at plan time.
bool NavigationManager::segmentIsClear(const Point& from, const Point& to) const {
    return navGeoSegmentIsClearZones(noGoLines, zonePolygons, zoneFilterActive, from, to);
}

size_t NavigationManager::findNearestWaypoint(float x, float y) const {
    Point current{x, y};
    size_t best = waypointIndex; // Sentinel: "no clear alternative found"
    float bestDistSq = -1.0f;
    for (size_t i = waypointIndex + 1; i < waypoints.size(); i++) {
        if (!segmentIsClear(current, waypoints[i].pt))
            continue; // Geometrically blocked from here — not a safe reroute target
        float dx = waypoints[i].pt.x - x;
        float dy = waypoints[i].pt.y - y;
        float d = dx * dx + dy * dy;
        if (bestDistSq < 0.0f || d < bestDistSq) {
            bestDistSq = d;
            best = i;
        }
    }
    return best;
}

// -- Lifecycle ------------------------------------------------------------------

bool NavigationManager::start(const String& forSession, const std::vector<String>& zoneLabels, String& error) {
    error = "";
    if (isActive()) {
        error = "guided clean already running";
        return false;
    }

    // Applies to every caller, not just the scheduler — nothing else stops a stale/direct
    // API call from starting against a session the user has since unpinned.
    if (!zonesMgr.isPinned(forSession)) {
        error = "reference session is not pinned";
        return false;
    }

    waypoints.clear();
    waypointIndex = 0;
    rerouteCount = 0;
    escapeAttemptsForThisBlock = 0;
    hasPose = false;
    lastError = "";
    noGoLines.clear();
    zonePolygons.clear();
    zoneFilterActive = false;
    traveledDistanceM = 0.0f;
    waypointsSkipPending = false;
    routeTruncated = false;
    plannedPathM = 0.0f;

    history.setGuidedActive(true);
    history.notifyCleanStart(); // start recording now, not on the next idle poll
    if (!loadWaypoints(forSession, error))
        return false;

    if (!applyZoneFilterAndNoGo(forSession, zoneLabels, error))
        return false;

    if (waypoints.empty()) {
        error = "no usable waypoints after zone filtering";
        return false;
    }

    std::vector<Point> plannedPts;
    plannedPts.reserve(waypoints.size());
    for (const auto& wp: waypoints)
        plannedPts.push_back(wp.pt);

    // Approach leg (dock -> first waypoint) counts against the budget at plan time only —
    // never added to `waypoints` itself, so it changes nothing about what's actually driven.
    std::vector<Point> withApproach;
    withApproach.reserve(plannedPts.size() + 1);
    withApproach.push_back(recordingFirstPoint);
    withApproach.insert(withApproach.end(), plannedPts.begin(), plannedPts.end());
    plannedPathM = navGeoPathLength(withApproach);

    if (plannedPathM > NAV_MGR_MAX_ODOM_PATH_M) {
        size_t keep = navGeoTruncateToBudget(recordingFirstPoint, plannedPts, NAV_MGR_MAX_ODOM_PATH_M);
        if (keep == 0) {
            float dx = plannedPts[0].x - recordingFirstPoint.x;
            float dy = plannedPts[0].y - recordingFirstPoint.y;
            float d = sqrtf(dx * dx + dy * dy);
            error = "nearest waypoint in the selected zone is " + String(d, 1) + " m from the dock — beyond the " +
                    String(NAV_MGR_MAX_ODOM_PATH_M, 0) +
                    " m odometry budget; pick a zone closer to the dock, or clean the whole house instead";
            return false;
        }
        waypoints.resize(keep);
        routeTruncated = true;
        LOG("GUIDED", "Truncating route to fit odometry budget: %.1fm planned, keeping %u of %u waypoints",
            plannedPathM, static_cast<unsigned>(keep), static_cast<unsigned>(plannedPts.size()));
        dataLogger.logGenericEvent("guided_truncated",
                                   {{"plannedPathM", String(plannedPathM, 1), FIELD_FLOAT},
                                    {"kept", String(static_cast<unsigned>(keep)), FIELD_INT},
                                    {"total", String(static_cast<unsigned>(plannedPts.size())), FIELD_INT}});
    }

    if (!navGeoStartsNearOrigin(recordingFirstPoint, NAV_MGR_ORIGIN_TOLERANCE_M)) {
        float d = sqrtf(recordingFirstPoint.x * recordingFirstPoint.x + recordingFirstPoint.y * recordingFirstPoint.y);
        error = "recorded session doesn't start at the dock (first point is " + String(d, 2) +
                " m from origin) — pin a session recorded from a full dock-to-dock clean";
        return false;
    }

    sessionName = forSession;
    LOG("GUIDED", "Starting guided clean: session=%s waypoints=%u zones=%d plannedPathM=%.1fm truncated=%d",
        forSession.c_str(), static_cast<unsigned>(waypoints.size()), static_cast<int>(zoneLabels.size()), plannedPathM,
        routeTruncated ? 1 : 0);

    setState(GUIDED_VALIDATING);
    waitingOnCallback = true;
    neato.getCharger([this](bool ok, const ChargerData& c) {
        waitingOnCallback = false;
        if (state != GUIDED_VALIDATING)
            return; // stop() ran while checking
        if (!ok) {
            fail("could not confirm dock status");
            return;
        }
        if (!c.extPwrPresent) {
            fail("guided clean must start from the dock (robot not on charger)");
            return;
        }
        beginUndock();
    });
    return true;
}

void NavigationManager::stop() {
    if (state == GUIDED_IDLE)
        return;
    history.setGuidedActive(false);
    LOG("GUIDED", "Stopped by request (state=%s, waypoint %u/%u)", stateName(state),
        static_cast<unsigned>(waypointIndex), static_cast<unsigned>(waypoints.size()));
    if (ownsManualMode) {
        manualMgr.move(0, 0, 0, nullptr);
        applyMotorPolicy(GUIDED_IDLE);
        manualMgr.enable(false, nullptr);
        manualMgr.setClientWatchdogSuppressed(false);
        ownsManualMode = false;
    }
    waitingOnCallback = false;
    setState(GUIDED_IDLE);
}

void NavigationManager::fail(const String& reason) {
    history.setGuidedActive(false);
    LOG("GUIDED", "Error: %s", reason.c_str());
    dataLogger.logGenericEvent("guided_fail",
                               {{"reason", reason, FIELD_STRING},
                                {"state", stateName(state), FIELD_STRING},
                                {"waypointIndex", String(static_cast<unsigned>(waypointIndex)), FIELD_INT},
                                {"waypointCount", String(static_cast<unsigned>(waypoints.size())), FIELD_INT}});
    lastError = reason;
    if (ownsManualMode) {
        manualMgr.move(0, 0, 0, nullptr);
        applyMotorPolicy(GUIDED_IDLE);
        manualMgr.enable(false, nullptr);
        manualMgr.setClientWatchdogSuppressed(false);
        ownsManualMode = false;
    }
    waitingOnCallback = false;
    setState(GUIDED_ERROR);
}

// Single point where motor state follows the policy, so the policy function and
// the real calls can't drift apart.
void NavigationManager::applyMotorPolicy(GuidedCleanState forState) {
    bool on = guidedMotorsShouldRun(forState);
    manualMgr.setMotors(on, on, on, [on](bool ok) {
        if (!ok)
            LOG("GUIDED", "setMotors %s failed", on ? "on" : "off");
    });
}

void NavigationManager::setState(GuidedCleanState s) {
    if (state != s) {
        LOG("GUIDED", "State: %s -> %s", stateName(state), stateName(s));
        dataLogger.logGenericEvent("guided_state",
                                   {{"from", stateName(state), FIELD_STRING},
                                    {"to", stateName(s), FIELD_STRING},
                                    {"waypointIndex", String(static_cast<unsigned>(waypointIndex)), FIELD_INT},
                                    {"waypointCount", String(static_cast<unsigned>(waypoints.size())), FIELD_INT}});
    }
    state = s;
}

// Treats an immediate move() rejection the same as a callback failure — move() returns
// false WITHOUT calling back when manual mode isn't active, which would otherwise leave
// waitingOnCallback stuck true forever.
void NavigationManager::issueMove(int leftMM, int rightMM, int speedMMs, const char *rejectReason) {
    waitingOnCallback = true;
    bool accepted = manualMgr.move(leftMM, rightMM, speedMMs, [this](bool ok) {
        waitingOnCallback = false;
        if (!ok) {
            MoveBlockReason r = manualMgr.lastBlockReason();
            handleBlocked(moveBlockReasonToString(r), r);
        }
    });
    if (!accepted) {
        waitingOnCallback = false;
        fail(rejectReason);
    }
}

void NavigationManager::beginUndock() {
    setState(GUIDED_UNDOCKING);
    routeStartedAtMs = millis();
    waitingOnCallback = true;
    bool accepted = manualMgr.enable(true, [this](bool ok) {
        // stop() may have run while acquiring — release and bail
        if (state != GUIDED_UNDOCKING) {
            waitingOnCallback = false;
            if (ok)
                manualMgr.enable(false, nullptr);
            LOG("GUIDED", "Undock acquisition resolved after cancellation — discarding");
            return;
        }
        if (!ok) {
            waitingOnCallback = false;
            fail("failed to enter manual mode for undock");
            return;
        }
        ownsManualMode = true;
        manualMgr.setClientWatchdogSuppressed(true);
        // UNVALIDATED: assumes robot backs onto dock, so undock drives forward — confirm on real D7.
        // moveRelaxedStall(): undock's breakaway effort measured 60-61% wheel load against
        // free-driving obstruction thresholds -- a one-shot bounded move, not a general drive,
        // so wheel-load stall is skipped here specifically; bumper/drop safety stay active.
        int mm = static_cast<int>(roundf(NAV_MGR_UNDOCK_DIST_M * 1000.0f));
        waitingOnCallback = true;
        bool moveAccepted = manualMgr.moveRelaxedStall(mm, mm, NAV_DRIVE_SPEED_MMS, [this](bool moveOk) {
            waitingOnCallback = false;
            if (!moveOk) {
                fail(String("undock move blocked: ") + moveBlockReasonToString(manualMgr.lastBlockReason()));
                return;
            }
            pollTicker.reset();
            batteryTicker.reset();
            waypointsSkipPending = true;
            applyMotorPolicy(GUIDED_ROTATING);
            setState(GUIDED_ROTATING);
        });
        if (!moveAccepted) {
            waitingOnCallback = false;
            fail("undock move rejected (manual mode inactive)");
        }
    });
    // enable() calls back synchronously on rejection, so `accepted` is just a defensive check here.
    if (!accepted) {
        waitingOnCallback = false;
    }
}

// -- Main loop --------------------------------------------------------------------

void NavigationManager::tick() {
    if (state == GUIDED_IDLE || state == GUIDED_DONE || state == GUIDED_ERROR)
        return;
    if (waitingOnCallback)
        return; // A move()/enable()/clean() call is in flight — don't overlap commands

    if (state == GUIDED_CHARGING) {
        if (batteryTicker.elapsed(NAV_MGR_BATTERY_CHECK_MS))
            checkBattery();
        return;
    }

    bool navigating = (state == GUIDED_ROTATING || state == GUIDED_DRIVING || state == GUIDED_CORRECTING);

    // Stale pose watchdog — don't keep driving blind if position fixes stop arriving.
    if (navigating && hasPose && millis() - lastPoseAtMs > NAV_STALE_POSE_MS) {
        fail("no position fix for too long");
        return;
    }

    // Odometry guards — no re-localization ever corrects drift, so both are hard stops.
    if (navigating && traveledDistanceM > NAV_MGR_MAX_ODOM_PATH_M) {
        fail("exceeded odometry path guard mid-route (rerouting)");
        return;
    }
    if (navigating && millis() - routeStartedAtMs > NAV_MGR_MAX_ODOM_DURATION_MS) {
        fail("exceeded odometry duration guard");
        return;
    }

    if (batteryTicker.elapsed(NAV_MGR_BATTERY_CHECK_MS))
        checkBattery();

    if (navigating && pollTicker.elapsed(NAV_POLL_MS)) {
        pollPose();
    }
}

void NavigationManager::pollPose() {
    if (fetchInFlight)
        return;
    fetchInFlight = true;
    neato.getRobotPos(false, [this](bool ok, const RobotPosData& pos) {
        fetchInFlight = false;
        if (!isActive())
            return;
        if (!ok || !pos.hasPose) {
            LOG("GUIDED", "GetRobotPos fetch/parse failed, will retry next poll");
            return;
        }
        lastX = pos.x;
        lastY = pos.y;
        lastTheta = pos.theta;
        hasPose = true;
        lastPoseAtMs = millis();

        if (waypointsSkipPending) {
            waypointsSkipPending = false;
            std::vector<Point> remaining;
            remaining.reserve(waypoints.size() - waypointIndex);
            for (size_t i = waypointIndex; i < waypoints.size(); i++)
                remaining.push_back(waypoints[i].pt);
            size_t skip = navGeoSkipCoveredWaypoints(remaining, Point{lastX, lastY}, NAV_MGR_WAYPOINT_SKIP_RADIUS_M);
            if (skip > 0) {
                LOG("GUIDED", "Skipping %u already-covered waypoint(s) near undock position",
                    static_cast<unsigned>(skip));
                waypointIndex += skip;
            }
            if (waypointIndex >= waypoints.size())
                LOG("GUIDED", "Whole route was within the undock radius — nothing left to drive");
        }

        driveTowardCurrentWaypoint();
    });
}

void NavigationManager::driveTowardCurrentWaypoint() {
    if (waypointIndex >= waypoints.size()) {
        finishRoute();
        return;
    }

    GuidedWaypoint& wp = waypoints[waypointIndex];
    Point current{lastX, lastY};
    if (!segmentIsClear(current, wp.pt)) {
        handleBlocked("no-go/zone boundary crossing on approach");
        return;
    }

    float dx = wp.pt.x - lastX;
    float dy = wp.pt.y - lastY;
    float dist = sqrtf(dx * dx + dy * dy);

    if (dist <= NAV_ARRIVAL_THRESHOLD_M) {
        advanceWaypoint();
        return;
    }

    float targetHeadingDeg = atan2f(dy, dx) * kRadToDeg;
    float headingErr = normalizeDeg(targetHeadingDeg - lastTheta);

    if (fabsf(headingErr) > NAV_HEADING_TOLERANCE_DEG) {
        setState(GUIDED_ROTATING);
        float step = headingErr;
        if (step > NAV_MAX_ROTATE_STEP_DEG)
            step = NAV_MAX_ROTATE_STEP_DEG;
        if (step < -NAV_MAX_ROTATE_STEP_DEG)
            step = -NAV_MAX_ROTATE_STEP_DEG;

        float arcLenMm = NAV_TRACK_WIDTH_MM * static_cast<float>(PI) * (fabsf(step) / 360.0f);
        int mag = static_cast<int>(roundf(arcLenMm));
        int sign = (step >= 0.0f ? 1 : -1) * NAV_ROTATE_SIGN;
        int leftMM = -sign * mag;
        int rightMM = sign * mag;

        // Smoothing (spec §4.3): a small heading correction gets a slower turn instead of
        // always the full-step speed, so it reads as a controlled nudge, not a hard pivot.
        int rotateSpeed =
                scaledSpeedMms(fabsf(step), NAV_MAX_ROTATE_STEP_DEG, NAV_ROTATE_MIN_SPEED_MMS, NAV_ROTATE_SPEED_MMS);
        issueMove(leftMM, rightMM, rotateSpeed, "rotate move rejected (manual mode inactive)");
    } else {
        setState(GUIDED_DRIVING);
        float step = fminf(dist, NAV_MAX_DRIVE_STEP_M);
        int mm = static_cast<int>(roundf(step * 1000.0f));
        checkLidarThenDrive(mm);
    }
}

// LIDAR proximity veto (careful-driving spec §2): scans only here, in the stationary poll
// gap, never mid-drive. Falls back to the full requested step (bumper/stall backstop) on a
// failed fetch or an unreliable sector, per the spec's "don't invent a distance" rule.
void NavigationManager::checkLidarThenDrive(int requestedMm) {
    waitingOnCallback = true;
    neato.getLdsScan([this, requestedMm](bool ok, const LdsScanData& scan) {
        waitingOnCallback = false;
        int stepMm = requestedMm;
        if (!ok) {
            LOG("GUIDED", "GetLDSScan fetch failed, driving the full requested step (bumper/stall backstop)");
        } else {
            int distMm[LDS_NUM_POINTS];
            int errCode[LDS_NUM_POINTS];
            for (int i = 0; i < LDS_NUM_POINTS; i++) {
                distMm[i] = scan.points[i].distMM;
                errCode[i] = scan.points[i].errorCode;
            }
            LdsSectorResult sector = ldsSectorMinDistance(distMm, errCode, NAV_LDS_FRONT_OFFSET_DEG, 0,
                                                          NAV_LDS_DRIVE_SECTOR_HALF_DEG, NAV_LDS_MIN_VALID_SAMPLES);
            if (!sector.reliable) {
                dataLogger.logGenericEvent("lds_sector_unreliable",
                                           {{"validSampleCount", String(sector.validSampleCount), FIELD_INT}});
            }
            LdsDriveDecision decision = ldsDriveDecision(sector, requestedMm, NAV_LDS_FOOTPRINT_RADIUS_MM,
                                                         NAV_LDS_STOP_MARGIN_MM, NAV_LDS_MIN_USEFUL_STEP_MM);
            if (decision.action == LdsDriveAction::Stop) {
                dataLogger.logGenericEvent("lds_preemptive_block", {{"minDistMm", String(sector.minDistMm), FIELD_INT},
                                                                    {"requestedMm", String(requestedMm), FIELD_INT}});
                handleBlocked("lidar proximity veto", MoveBlockReason::kLidarProximity);
                return;
            }
            stepMm = decision.adjustedStepMm;
        }
        traveledDistanceM += static_cast<float>(stepMm) / 1000.0f;
        // Smoothing (spec §4.3): scale speed to how much of the full step this actually is --
        // a shortened final-approach or LIDAR-shortened step gets a slower, controlled arrival
        // instead of full speed followed by a hard stop. No odometry cost: drift scales with
        // distance travelled, not speed/time.
        int driveSpeed = scaledSpeedMms(static_cast<float>(stepMm), NAV_MAX_DRIVE_STEP_M * 1000.0f,
                                        NAV_DRIVE_MIN_SPEED_MMS, NAV_DRIVE_SPEED_MMS);
        issueMove(stepMm, stepMm, driveSpeed, "drive move rejected (manual mode inactive)");
    });
}

void NavigationManager::advanceWaypoint() {
    rerouteCount = 0;
    escapeAttemptsForThisBlock = 0;
    waypointIndex++;
    LOG("GUIDED", "Waypoint %u/%u reached", static_cast<unsigned>(waypointIndex),
        static_cast<unsigned>(waypoints.size()));
    if (waypointIndex >= waypoints.size())
        finishRoute();
    // Otherwise: the next scheduled poll tick evaluates the new current waypoint.
}

// Entry point for every block (physical or geometric). Physical reasons get one escape
// attempt (reverse/turn-away, spec §3) before falling through to the existing geometric
// reroute; typedReason defaults to kNone (no escape) for the no-go/zone-crossing and LIDAR
// pre-veto call sites, which have nothing physical to move away from.
void NavigationManager::handleBlocked(const char *reason, MoveBlockReason typedReason) {
    EscapeMotion escape = escapeMotionFor(typedReason, NAV_MGR_ESCAPE_REVERSE_MM, NAV_MGR_ESCAPE_ROTATE_DEG);
    if (escape.hasEscape && escapeAttemptsForThisBlock < NAV_MGR_MAX_ESCAPE_ATTEMPTS) {
        escapeAttemptsForThisBlock++;
        beginEscape(reason, escape);
        return;
    }
    rerouteAfterBlock(reason);
}

void NavigationManager::rerouteAfterBlock(const char *reason) {
    setState(GUIDED_CORRECTING);
    if (rerouteCount >= NAV_MGR_MAX_REROUTES) {
        fail(String("blocked and reroute budget exhausted: ") + reason);
        return;
    }
    // findNearestWaypoint()'s sentinel (alt == waypointIndex) must be disambiguated: route
    // genuinely ending here (soft skip) vs. waypoints remain but all are blocked (hard fail).
    bool anyRemaining = (waypointIndex + 1) < waypoints.size();
    size_t alt = findNearestWaypoint(lastX, lastY);
    if (alt == waypointIndex) {
        if (!anyRemaining) {
            LOG("GUIDED", "%s, route ends here — skipping final waypoint", reason);
            dataLogger.logGenericEvent("guided_blocked",
                                       {{"reason", reason, FIELD_STRING}, {"result", "skip_final", FIELD_STRING}});
            advanceWaypoint();
            return;
        }
        // Fail loudly instead of skipping — a silent skip would never hit
        // NAV_MGR_MAX_REROUTES and would falsely report "route complete".
        fail(String("blocked: no clear path to any remaining waypoint (") + reason + ")");
        return;
    }
    LOG("GUIDED", "%s — rerouting to waypoint %u", reason, static_cast<unsigned>(alt));
    dataLogger.logGenericEvent("guided_blocked",
                               {{"reason", reason, FIELD_STRING},
                                {"result", "reroute", FIELD_STRING},
                                {"fromWaypoint", String(static_cast<unsigned>(waypointIndex)), FIELD_INT},
                                {"toWaypoint", String(static_cast<unsigned>(alt)), FIELD_INT}});
    waypointIndex = alt;
    rerouteCount++;
    // Next scheduled poll tick re-evaluates against the new target.
}

// Reverse (or forward, kStallRear's mirror) leg. Full stall detection stays active (unlike
// beginUndock's moveRelaxedStall) -- per spec §3.5, a blocked reverse can only mean something
// is directly behind the robot too (no rear bumper on this hardware), i.e. genuinely boxed in.
void NavigationManager::beginEscape(const char *reason, const EscapeMotion& escape) {
    setState(GUIDED_ESCAPING);
    LOG("GUIDED", "Escaping (%s): leg=%dmm%s", reason, escape.firstLegMm,
        escape.hasTurn ? (escape.turnDir == EscapeTurnDir::Left ? " then turn left" : " then turn right") : "");
    dataLogger.logGenericEvent("guided_escape_start", {{"reason", reason, FIELD_STRING},
                                                       {"attempt", String(escapeAttemptsForThisBlock), FIELD_INT},
                                                       {"firstLegMm", String(escape.firstLegMm), FIELD_INT},
                                                       {"hasTurn", escape.hasTurn ? "true" : "false", FIELD_BOOL}});
    waitingOnCallback = true;
    bool accepted =
            manualMgr.move(escape.firstLegMm, escape.firstLegMm, NAV_DRIVE_SPEED_MMS, [this, reason, escape](bool ok) {
                waitingOnCallback = false;
                if (!isActive())
                    return; // stop()/fail() ran while the escape leg was in flight
                if (!ok) {
                    MoveBlockReason r = manualMgr.lastBlockReason();
                    LOG("GUIDED", "Escape leg blocked (%s) — boxed in", moveBlockReasonToString(r));
                    dataLogger.logGenericEvent("guided_escape_boxed_in",
                                               {{"reason", reason, FIELD_STRING},
                                                {"blockReason", moveBlockReasonToString(r), FIELD_STRING}});
                    fail(String("escape blocked: boxed in (front+rear both obstructed) — ") + reason);
                    return;
                }
                if (escape.hasTurn)
                    issueEscapeTurn(reason, escape.turnDir, escape.rotateDeg);
                else
                    finishEscape(reason, /*turnCompleted=*/true);
            });
    if (!accepted) {
        waitingOnCallback = false;
        fail("escape move rejected (manual mode inactive)");
    }
}

// Turn-away leg. Same differential-wheel arc math as driveTowardCurrentWaypoint's rotate step.
void NavigationManager::issueEscapeTurn(const char *reason, EscapeTurnDir dir, int rotateDeg) {
    float arcLenMm = NAV_TRACK_WIDTH_MM * static_cast<float>(PI) * (static_cast<float>(rotateDeg) / 360.0f);
    int mag = static_cast<int>(roundf(arcLenMm));
    int sign = (dir == EscapeTurnDir::Left ? 1 : -1) * NAV_ROTATE_SIGN;
    int leftMM = -sign * mag;
    int rightMM = sign * mag;

    waitingOnCallback = true;
    bool accepted = manualMgr.move(leftMM, rightMM, NAV_ROTATE_SPEED_MMS, [this, reason](bool ok) {
        waitingOnCallback = false;
        if (!isActive())
            return; // stop()/fail() ran while the turn was in flight
        if (!ok) {
            // The reverse already bought physical clearance -- a blocked turn-away doesn't
            // undo that, so log and proceed to reroute rather than failing the route.
            MoveBlockReason r = manualMgr.lastBlockReason();
            LOG("GUIDED", "Escape turn blocked (%s) — proceeding to reroute anyway", moveBlockReasonToString(r));
            dataLogger.logGenericEvent(
                    "guided_escape_turn_blocked",
                    {{"reason", reason, FIELD_STRING}, {"blockReason", moveBlockReasonToString(r), FIELD_STRING}});
        }
        finishEscape(reason, /*turnCompleted=*/ok);
    });
    if (!accepted) {
        waitingOnCallback = false;
        finishEscape(reason, /*turnCompleted=*/false);
    }
}

void NavigationManager::finishEscape(const char *reason, bool turnCompleted) {
    if (!isActive())
        return;
    dataLogger.logGenericEvent("guided_escape_done", {{"reason", reason, FIELD_STRING},
                                                      {"turnCompleted", turnCompleted ? "true" : "false", FIELD_BOOL}});
    rerouteAfterBlock(reason);
}

// -- Dock / charge transitions ----------------------------------------------------

void NavigationManager::finishRoute() {
    history.setGuidedActive(false);
    LOG("GUIDED", "Route complete (%s) — returning to dock",
        routeTruncated ? "truncated for odometry budget" : "full route");
    manualMgr.move(0, 0, 0, nullptr);
    applyMotorPolicy(GUIDED_IDLE);
    waitingOnCallback = true;
    setState(GUIDED_DOCKING);
    manualMgr.setClientWatchdogSuppressed(false);
    ownsManualMode = false;
    manualMgr.enable(false, [this](bool) {
        // Never `Clean Stop` — clean("dock")'s SetEvent path preserves localization.
        neato.clean("dock", 0, 0, [this](bool ok) {
            waitingOnCallback = false;
            if (!ok) {
                fail("failed to send dock command after route completion");
                return;
            }
            setState(GUIDED_DONE);
        });
    });
}

void NavigationManager::checkBattery() {
    neato.getCharger([this](bool ok, const ChargerData& c) {
        if (!ok || c.fuelPercent < 0)
            return;
        bool navigating = (state == GUIDED_ROTATING || state == GUIDED_DRIVING || state == GUIDED_CORRECTING);
        // Resuming would re-zero Raw odometry at an unverified dock position and
        // break the waypoint frame, so end the run instead.
        if (navigating && c.fuelPercent <= NAV_MGR_LOW_BATTERY_PCT) {
            fail("battery low — guided run ended (routes are short by design)");
        }
    });
}

// -- Status -----------------------------------------------------------------------

String NavigationManager::getStatusJson() const {
    return fieldsToJson({
            {"active", isActive() ? "true" : "false", FIELD_BOOL},
            {"state", stateName(state), FIELD_STRING},
            {"session", sessionName, FIELD_STRING},
            {"waypointIndex", String(static_cast<unsigned>(waypointIndex)), FIELD_INT},
            {"waypointCount", String(static_cast<unsigned>(waypoints.size())), FIELD_INT},
            {"hasPose", hasPose ? "true" : "false", FIELD_BOOL},
            {"lastX", String(lastX, 3), FIELD_FLOAT},
            {"lastY", String(lastY, 3), FIELD_FLOAT},
            {"lastTheta", String(lastTheta, 1), FIELD_FLOAT},
            {"rerouteCount", String(rerouteCount), FIELD_INT},
            {"traveledDistanceM", String(traveledDistanceM, 1), FIELD_FLOAT},
            {"truncated", routeTruncated ? "true" : "false", FIELD_BOOL},
            {"plannedPathM", String(plannedPathM, 1), FIELD_FLOAT},
            {"error", lastError, FIELD_STRING},
    });
}
