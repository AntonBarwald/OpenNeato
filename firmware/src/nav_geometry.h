#ifndef NAV_GEOMETRY_H
#define NAV_GEOMETRY_H

#include <vector>
#include "geometry.h"

// Zone/no-go clearance checks backing NavigationManager::segmentIsClear(); kept out of
// geometry.h (which stays in lockstep with the frontend port) since only firmware needs this.
// Zero Arduino dependency, so it's host-testable — see nav_geometry_selftest.cpp.

// Closed-polygon analogue of geometry.h's polylineCrossesSegment, including the closing edge.
inline bool zoneBoundaryCrossesSegment(const std::vector<Point>& polygon, const Point& a, const Point& b) {
    size_t n = polygon.size();
    for (size_t i = 0, j = n - 1; i < n; j = i++) {
        if (segmentsIntersect(a, b, polygon[j], polygon[i]))
            return true;
    }
    return false;
}

// True if from->to crosses no line in noGoLines and (if zoneFilterActive) `to` lies inside at
// least one polygon in zonePolygons; multiple zonePolygons means union, not intersection.
// A zone defines WHERE TO CLEAN, not where the robot may travel, so the no-notch-cutting
// boundary check only applies once `from` is ALSO already inside that same polygon — entering
// the zone from outside (e.g. right after undocking at its edge) necessarily crosses the
// boundary once, which is fine; only wandering out through a concave notch while already
// working inside the zone is blocked.
// `to`'s zone membership is re-checked here rather than trusted from plan time — cheap, and
// safety paths shouldn't rely solely on an upstream invariant holding.
inline bool navGeoSegmentIsClearZones(const std::vector<std::vector<Point>>& noGoLines,
                                      const std::vector<std::vector<Point>>& zonePolygons, bool zoneFilterActive,
                                      const Point& from, const Point& to) {
    for (const auto& line: noGoLines) {
        if (polylineCrossesSegment(line, from, to))
            return false;
    }
    if (zoneFilterActive) {
        bool anyOk = false;
        for (const auto& poly: zonePolygons) {
            if (!pointInPolygon(to, poly))
                continue;
            if (pointInPolygon(from, poly) && zoneBoundaryCrossesSegment(poly, from, to))
                continue; // already inside this zone — cutting outside through a notch is blocked
            anyOk = true;
            break;
        }
        if (!anyOk)
            return false;
    }
    return true;
}

// Sum of consecutive waypoint-to-waypoint straight-line distances — the planned path length
// backing NavigationManager's odometry-only route-length guard (no re-localization to
// correct drift, so a long route is out of scope; see NAV_MGR_MAX_ODOM_PATH_M).
inline float navGeoPathLength(const std::vector<Point>& waypoints) {
    float total = 0.0f;
    for (size_t i = 1; i < waypoints.size(); i++) {
        float dx = waypoints[i].x - waypoints[i - 1].x;
        float dy = waypoints[i].y - waypoints[i - 1].y;
        total += sqrtf(dx * dx + dy * dy);
    }
    return total;
}

// Count of leading `waypoints` whose cumulative distance from `from` fits within `budgetM` —
// backs NavigationManager::start()'s truncate-instead-of-refuse path (see NAV_MGR_MAX_ODOM_PATH_M).
// 0 if even the from->waypoints[0] leg alone exceeds the budget.
inline size_t navGeoTruncateToBudget(const Point& from, const std::vector<Point>& waypoints, float budgetM) {
    float cum = 0.0f;
    Point prev = from;
    for (size_t i = 0; i < waypoints.size(); i++) {
        float dx = waypoints[i].x - prev.x;
        float dy = waypoints[i].y - prev.y;
        cum += sqrtf(dx * dx + dy * dy);
        if (cum > budgetM)
            return i;
        prev = waypoints[i];
    }
    return waypoints.size();
}

// Downsamples a full raw pose trail to at most maxWaypoints points, keeping every point at
// least minDistM from the last kept one. If that plain rule would keep more than maxWaypoints
// points, the threshold is grown adaptively (retrying over the WHOLE trail each time) so the
// retained set still spans start-to-end rather than only a scanned prefix. Always force-keeps
// the trail's true final point. Pure/host-testable — no Arduino/file I/O dependency.
inline std::vector<Point> navGeoDownsampleTrail(const std::vector<Point>& raw, float minDistM, size_t maxWaypoints) {
    if (raw.empty() || maxWaypoints == 0)
        return {};

    auto downsampleAt = [&](float threshold) {
        std::vector<Point> kept;
        kept.push_back(raw.front());
        for (size_t i = 1; i < raw.size(); i++) {
            float dx = raw[i].x - kept.back().x;
            float dy = raw[i].y - kept.back().y;
            if (sqrtf(dx * dx + dy * dy) >= threshold)
                kept.push_back(raw[i]);
        }
        return kept;
    };

    float threshold = minDistM;
    std::vector<Point> kept = downsampleAt(threshold);
    for (int guard = 0; kept.size() > maxWaypoints && guard < 30; guard++) {
        threshold *= 1.5f;
        kept = downsampleAt(threshold);
    }

    // Defensive fallback for pathological distributions where threshold growth alone doesn't
    // converge in time: evenly stride the over-budget result so the hard cap always holds.
    if (kept.size() > maxWaypoints) {
        std::vector<Point> strided;
        if (maxWaypoints == 1) {
            strided.push_back(kept.back());
        } else {
            float step = static_cast<float>(kept.size() - 1) / static_cast<float>(maxWaypoints - 1);
            for (size_t i = 0; i < maxWaypoints; i++) {
                size_t idx = static_cast<size_t>(lroundf(static_cast<float>(i) * step));
                if (idx >= kept.size())
                    idx = kept.size() - 1;
                strided.push_back(kept[idx]);
            }
        }
        kept = strided;
    }

    // The route should always end where the recording did, even if it falls below threshold.
    // `kept` always has >=1 element here (downsampleAt/strided both guarantee it).
    const Point& lastRaw = raw.back();
    if (kept.back().x != lastRaw.x || kept.back().y != lastRaw.y) {
        if (kept.size() >= maxWaypoints)
            kept.back() = lastRaw; // stay within cap — replace rather than append
        else
            kept.push_back(lastRaw);
    }

    return kept;
}

// True if `first` (a session's first recorded waypoint) is within toleranceM of the origin —
// the frame-alignment precondition for guided replay (the recorded frame and the TestMode
// Raw frame are both dock-anchored only if the session actually started at the dock).
inline bool navGeoStartsNearOrigin(const Point& first, float toleranceM) {
    return sqrtf(first.x * first.x + first.y * first.y) <= toleranceM;
}

// Returns the index of the first waypoint that is NOT within skipRadiusM of `current` —
// i.e. how many leading waypoints the undock move already covered and should be skipped.
// Only a leading run is ever skipped: once a waypoint falls outside the radius, scanning
// stops even if a later waypoint happens to be close again (a real route can loop near its
// start). Returns waypoints.size() if every waypoint is within the radius (nothing left to
// drive to).
inline size_t navGeoSkipCoveredWaypoints(const std::vector<Point>& waypoints, const Point& current, float skipRadiusM) {
    size_t i = 0;
    while (i < waypoints.size()) {
        float dx = waypoints[i].x - current.x;
        float dy = waypoints[i].y - current.y;
        if (sqrtf(dx * dx + dy * dy) > skipRadiusM)
            break;
        i++;
    }
    return i;
}

#endif // NAV_GEOMETRY_H
