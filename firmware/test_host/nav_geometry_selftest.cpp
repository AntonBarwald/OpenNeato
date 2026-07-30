// Host-only self-test for firmware/src/nav_geometry.h — kept out of PlatformIO's src_dir so
// it never enters the embedded build; compiles with plain clang++ (zero Arduino dependency).
// Covers zoneBoundaryCrossesSegment() and navGeoSegmentIsClearZones(), the free-function core
// of NavigationManager::segmentIsClear() that isn't otherwise host-testable.
//
// Run:
//   clang++ -std=c++17 -I firmware/src firmware/test_host/nav_geometry_selftest.cpp -o /tmp/navgeomtest && /tmp/navgeomtest

#include "nav_geometry.h"
#include <cstdio>

static int failures = 0;

static void expectTrue(bool cond, const char *name) {
    if (!cond) {
        std::printf("FAIL: %s\n", name);
        failures++;
    } else {
        std::printf("PASS: %s\n", name);
    }
}

int main() {
    // -- zoneBoundaryCrossesSegment: crosses one edge of a square ------------
    {
        std::vector<Point> square = {{0, 0}, {4, 0}, {4, 4}, {0, 4}};
        Point a{-1, 2}, b{1, 2}; // crosses the left edge at (0,2)
        expectTrue(zoneBoundaryCrossesSegment(square, a, b),
                   "zoneBoundaryCrossesSegment: crosses left edge of square");
    }

    // -- zoneBoundaryCrossesSegment: entirely inside, touches no edge --------
    {
        std::vector<Point> square = {{0, 0}, {4, 0}, {4, 4}, {0, 4}};
        Point a{1, 1}, b{3, 3};
        expectTrue(!zoneBoundaryCrossesSegment(square, a, b),
                   "zoneBoundaryCrossesSegment: fully interior segment touches no edge");
    }

    // -- zoneBoundaryCrossesSegment: crosses the CLOSING edge (last->first) --
    {
        std::vector<Point> square = {{0, 0}, {4, 0}, {4, 4}, {0, 4}}; // closing edge: (0,4)-(0,0)
        Point a{-1, 2}, b{0.5f, 2}; // crosses the closing (left) edge at (0,2)
        expectTrue(zoneBoundaryCrossesSegment(square, a, b),
                   "zoneBoundaryCrossesSegment: crosses closing edge (last vertex back to first)");
    }

    // -- zoneBoundaryCrossesSegment: entirely outside, no crossing -----------
    {
        std::vector<Point> square = {{0, 0}, {4, 0}, {4, 4}, {0, 4}};
        Point a{10, 10}, b{20, 20};
        expectTrue(!zoneBoundaryCrossesSegment(square, a, b),
                   "zoneBoundaryCrossesSegment: far-away segment touches no edge");
    }

    // -- navGeoSegmentIsClearZones: no no-go lines, no zone filter -> clear --
    {
        std::vector<std::vector<Point>> noGoLines;
        std::vector<std::vector<Point>> zonePolygons;
        expectTrue(navGeoSegmentIsClearZones(noGoLines, zonePolygons, false, {0, 0}, {5, 5}),
                   "navGeoSegmentIsClearZones: no geometry configured -> clear");
    }

    // -- navGeoSegmentIsClearZones: blocked by a no-go line -------------------
    {
        std::vector<std::vector<Point>> noGoLines = {{{2, -1}, {2, 5}}}; // vertical wall at x=2
        std::vector<std::vector<Point>> zonePolygons;
        expectTrue(!navGeoSegmentIsClearZones(noGoLines, zonePolygons, false, {0, 0}, {4, 0}),
                   "navGeoSegmentIsClearZones: segment crossing a no-go line is blocked");
        expectTrue(navGeoSegmentIsClearZones(noGoLines, zonePolygons, false, {0, 0}, {1, 0}),
                   "navGeoSegmentIsClearZones: segment not reaching the no-go line is clear");
    }

    // -- navGeoSegmentIsClearZones: multiple no-go lines, blocked by 2nd -----
    {
        std::vector<std::vector<Point>> noGoLines = {{{10, -1}, {10, 5}}, {{2, -1}, {2, 5}}};
        std::vector<std::vector<Point>> zonePolygons;
        expectTrue(!navGeoSegmentIsClearZones(noGoLines, zonePolygons, false, {0, 0}, {4, 0}),
                   "navGeoSegmentIsClearZones: blocked by second no-go line even though first doesn't cross");
    }

    // -- navGeoSegmentIsClearZones: single zone, destination outside ---------
    {
        std::vector<std::vector<Point>> noGoLines;
        std::vector<std::vector<Point>> zonePolygons = {{{0, 0}, {4, 0}, {4, 4}, {0, 4}}};
        expectTrue(!navGeoSegmentIsClearZones(noGoLines, zonePolygons, true, {1, 1}, {10, 10}),
                   "navGeoSegmentIsClearZones: destination outside the only zone polygon is blocked");
    }

    // -- navGeoSegmentIsClearZones: single zone, destination inside, but the -
    // -- straight segment leaves and re-enters the zone (crosses boundary) ---
    {
        std::vector<std::vector<Point>> noGoLines;
        // Notched L-shape: (0,0)-(4,0)-(4,2)-(2,2)-(2,4)-(0,4); notch is not interior.
        std::vector<std::vector<Point>> zonePolygons = {{{0, 0}, {4, 0}, {4, 2}, {2, 2}, {2, 4}, {0, 4}}};
        // Both endpoints individually read "inside" the L-shape, but the straight line
        // between them clips through the notch — a lone endpoint-in-polygon check would miss this.
        expectTrue(!navGeoSegmentIsClearZones(noGoLines, zonePolygons, true, {0.2f, 3.8f}, {3.9f, 1.0f}),
                   "navGeoSegmentIsClearZones: straight segment clipping through a zone's notch is blocked");
    }

    // -- navGeoSegmentIsClearZones: single zone, segment fully inside --------
    {
        std::vector<std::vector<Point>> noGoLines;
        std::vector<std::vector<Point>> zonePolygons = {{{0, 0}, {4, 0}, {4, 4}, {0, 4}}};
        expectTrue(navGeoSegmentIsClearZones(noGoLines, zonePolygons, true, {1, 1}, {3, 3}),
                   "navGeoSegmentIsClearZones: segment fully inside the only zone is clear");
    }

    // -- navGeoSegmentIsClearZones: zone filter AND a no-go line, both must --
    // -- pass ------------------------------------------------------------------
    {
        std::vector<std::vector<Point>> noGoLines = {{{2, -1}, {2, 5}}};
        std::vector<std::vector<Point>> zonePolygons = {{{0, 0}, {4, 0}, {4, 4}, {0, 4}}};
        // Inside the zone, but crosses the no-go line -> blocked.
        expectTrue(!navGeoSegmentIsClearZones(noGoLines, zonePolygons, true, {1, 1}, {3, 1}),
                   "navGeoSegmentIsClearZones: inside zone but crossing a no-go line is still blocked");
        // Inside the zone, doesn't cross the no-go line -> clear.
        expectTrue(navGeoSegmentIsClearZones(noGoLines, zonePolygons, true, {0.5f, 1}, {1.5f, 1}),
                   "navGeoSegmentIsClearZones: inside zone and clear of no-go lines is clear");
    }

    // -- navGeoSegmentIsClearZones: UNION of two disjoint zones — a segment --
    // -- entirely inside the SECOND selected zone (not the first) is clear ---
    {
        std::vector<std::vector<Point>> noGoLines;
        std::vector<std::vector<Point>> zonePolygons = {
                {{0, 0}, {2, 0}, {2, 2}, {0, 2}}, // zone A: kitchen, near origin
                {{10, 10}, {12, 10}, {12, 12}, {10, 12}}, // zone B: hallway, far away
        };
        expectTrue(navGeoSegmentIsClearZones(noGoLines, zonePolygons, true, {10.5f, 10.5f}, {11.5f, 11.5f}),
                   "navGeoSegmentIsClearZones: segment inside the second selected zone (union) is clear");
        // Nowhere near either selected zone -> blocked.
        expectTrue(!navGeoSegmentIsClearZones(noGoLines, zonePolygons, true, {50, 50}, {51, 51}),
                   "navGeoSegmentIsClearZones: segment outside every selected zone is blocked");
    }

    // -- navGeoSegmentIsClearZones: FIX 1 — approach from OUTSIDE a zone to a --
    // waypoint INSIDE it must be allowed (undocking next to a zone edge was
    // blocking every drive attempt; a zone is where to clean, not a travel cage).
    {
        std::vector<std::vector<Point>> noGoLines;
        std::vector<std::vector<Point>> zonePolygons = {{{0, 0}, {4, 0}, {4, 4}, {0, 4}}};
        // from is outside the square, to is inside — straight line necessarily crosses the
        // boundary once, which must no longer block the approach.
        expectTrue(navGeoSegmentIsClearZones(noGoLines, zonePolygons, true, {-1, 2}, {2, 2}),
                   "navGeoSegmentIsClearZones: FIX1 approach from outside the zone to a waypoint inside is allowed");
    }

    // -- navGeoSegmentIsClearZones: FIX 1 — a no-go line still blocks the ------
    // approach segment even though the zone relaxation would otherwise allow it.
    {
        std::vector<std::vector<Point>> noGoLines = {{{0.5f, -1}, {0.5f, 5}}}; // wall between from and to
        std::vector<std::vector<Point>> zonePolygons = {{{0, 0}, {4, 0}, {4, 4}, {0, 4}}};
        expectTrue(!navGeoSegmentIsClearZones(noGoLines, zonePolygons, true, {-1, 2}, {2, 2}),
                   "navGeoSegmentIsClearZones: FIX1 no-go line still blocks an otherwise-allowed approach");
    }

    // -- navGeoSegmentIsClearZones: FIX 1 — once BOTH endpoints are already ----
    // inside the zone, cutting outside through a notch is still blocked (the
    // approach relaxation must not weaken in-zone travel).
    {
        std::vector<std::vector<Point>> noGoLines;
        std::vector<std::vector<Point>> zonePolygons = {{{0, 0}, {4, 0}, {4, 2}, {2, 2}, {2, 4}, {0, 4}}};
        expectTrue(!navGeoSegmentIsClearZones(noGoLines, zonePolygons, true, {0.2f, 3.8f}, {3.9f, 1.0f}),
                   "navGeoSegmentIsClearZones: FIX1 both endpoints already inside — notch cut still blocked");
    }

    // -- navGeoSegmentIsClearZones: FIX 1 — travel fully inside the zone is ----
    // still allowed (unaffected by the approach relaxation).
    {
        std::vector<std::vector<Point>> noGoLines;
        std::vector<std::vector<Point>> zonePolygons = {{{0, 0}, {4, 0}, {4, 4}, {0, 4}}};
        expectTrue(navGeoSegmentIsClearZones(noGoLines, zonePolygons, true, {1, 1}, {3, 3}),
                   "navGeoSegmentIsClearZones: FIX1 segment fully inside the zone remains clear");
    }

    // -- navGeoSegmentIsClearZones: FIX 1 — inside the zone heading to a point -
    // genuinely outside every selected zone is still blocked (a zone still
    // bounds where the robot is *aiming*, only entry is relaxed).
    {
        std::vector<std::vector<Point>> noGoLines;
        std::vector<std::vector<Point>> zonePolygons = {{{0, 0}, {4, 0}, {4, 4}, {0, 4}}};
        expectTrue(!navGeoSegmentIsClearZones(noGoLines, zonePolygons, true, {1, 1}, {10, 10}),
                   "navGeoSegmentIsClearZones: FIX1 inside-zone to outside-every-zone destination still blocked");
    }

    // -- navGeoPathLength: empty and single-point lists -> 0 ------------------
    {
        expectTrue(navGeoPathLength({}) == 0.0f, "navGeoPathLength: empty list is 0");
        expectTrue(navGeoPathLength({{1, 1}}) == 0.0f, "navGeoPathLength: single point is 0");
    }

    // -- navGeoPathLength: sums consecutive segment lengths --------------------
    {
        std::vector<Point> pts = {{0, 0}, {3, 4}, {3, 0}}; // 5 + 4 = 9
        float len = navGeoPathLength(pts);
        expectTrue(len > 8.999f && len < 9.001f, "navGeoPathLength: sums consecutive segments (3-4-5 then straight back)");
    }

    // -- navGeoStartsNearOrigin: exact origin, within/at/outside tolerance -----
    {
        expectTrue(navGeoStartsNearOrigin({0, 0}, 0.3f), "navGeoStartsNearOrigin: exact origin is within tolerance");
        expectTrue(navGeoStartsNearOrigin({0.2f, 0.2f}, 0.3f),
                   "navGeoStartsNearOrigin: within tolerance (0.283m < 0.3m)");
        expectTrue(navGeoStartsNearOrigin({0.3f, 0.0f}, 0.3f), "navGeoStartsNearOrigin: exactly at tolerance is accepted");
        expectTrue(!navGeoStartsNearOrigin({1.0f, 0.0f}, 0.3f), "navGeoStartsNearOrigin: well outside tolerance is refused");
    }

    // -- navGeoSkipCoveredWaypoints: real hardware sample — 3 at-dock points ---
    // followed by a genuine route waypoint, current pose ~0.3m from origin post-undock.
    {
        std::vector<Point> waypoints = {
                {0.000f, -0.001f},
                {-0.008f, 0.002f},
                {0.120f, -0.095f},
                {1.500f, 0.400f}, // genuinely ahead — must not be skipped
        };
        size_t start = navGeoSkipCoveredWaypoints(waypoints, {0.3f, 0.0f}, 0.45f);
        expectTrue(start == 3, "navGeoSkipCoveredWaypoints: skips the 3 at-dock waypoints, stops at the real route");
    }

    // -- navGeoSkipCoveredWaypoints: nothing near current position -> no skip --
    {
        std::vector<Point> waypoints = {{2.0f, 2.0f}, {3.0f, 3.0f}};
        size_t start = navGeoSkipCoveredWaypoints(waypoints, {0.0f, 0.0f}, 0.45f);
        expectTrue(start == 0, "navGeoSkipCoveredWaypoints: first waypoint already ahead -> no skip");
    }

    // -- navGeoSkipCoveredWaypoints: every waypoint within radius -> whole list -
    {
        std::vector<Point> waypoints = {{0.1f, 0.0f}, {0.2f, 0.0f}, {0.3f, 0.1f}};
        size_t start = navGeoSkipCoveredWaypoints(waypoints, {0.0f, 0.0f}, 0.45f);
        expectTrue(start == waypoints.size(), "navGeoSkipCoveredWaypoints: entire short route within radius -> index past end");
    }

    // -- navGeoSkipCoveredWaypoints: only a LEADING run is skipped — a later ---
    // waypoint drifting back near the current position must not be skipped too.
    {
        std::vector<Point> waypoints = {{0.1f, 0.0f}, {2.0f, 2.0f}, {0.05f, 0.0f}};
        size_t start = navGeoSkipCoveredWaypoints(waypoints, {0.0f, 0.0f}, 0.45f);
        expectTrue(start == 1, "navGeoSkipCoveredWaypoints: stops at the first out-of-radius waypoint, "
                               "does not resume skipping later");
    }

    // -- navGeoSkipCoveredWaypoints: empty list -> 0 ----------------------------
    {
        std::vector<Point> waypoints;
        expectTrue(navGeoSkipCoveredWaypoints(waypoints, {0.0f, 0.0f}, 0.45f) == 0,
                   "navGeoSkipCoveredWaypoints: empty waypoint list returns 0");
    }

    // -- navGeoTruncateToBudget: budget comfortably covers whole route -------
    {
        std::vector<Point> waypoints = {{1, 0}, {2, 0}, {3, 0}}; // 1+1+1 = 3m from (0,0)
        size_t n = navGeoTruncateToBudget({0, 0}, waypoints, 10.0f);
        expectTrue(n == waypoints.size(), "navGeoTruncateToBudget: budget covers whole route -> all waypoints kept");
    }

    // -- navGeoTruncateToBudget: budget cuts mid-route ------------------------
    {
        std::vector<Point> waypoints = {{1, 0}, {2, 0}, {3, 0}, {4, 0}}; // cumulative from origin: 1,2,3,4
        size_t n = navGeoTruncateToBudget({0, 0}, waypoints, 2.5f);
        expectTrue(n == 2, "navGeoTruncateToBudget: budget cuts mid-route -> correct prefix count");
        std::vector<Point> kept(waypoints.begin(), waypoints.begin() + static_cast<long>(n));
        std::vector<Point> withFrom = {{0, 0}};
        withFrom.insert(withFrom.end(), kept.begin(), kept.end());
        float keptLen = navGeoPathLength(withFrom);
        expectTrue(keptLen <= 2.5f, "navGeoTruncateToBudget: kept prefix length is within budget");
        std::vector<Point> withNext = {{0, 0}, waypoints[0], waypoints[1], waypoints[2]};
        expectTrue(navGeoPathLength(withNext) > 2.5f, "navGeoTruncateToBudget: adding the next waypoint would exceed budget");
    }

    // -- navGeoTruncateToBudget: budget smaller than the from->waypoints[0] leg --
    {
        std::vector<Point> waypoints = {{5, 0}, {6, 0}};
        size_t n = navGeoTruncateToBudget({0, 0}, waypoints, 2.0f);
        expectTrue(n == 0, "navGeoTruncateToBudget: budget too small even for the first leg -> 0");
    }

    // -- navGeoTruncateToBudget: budget lands exactly on a cumulative boundary --
    {
        std::vector<Point> waypoints = {{1, 0}, {2, 0}, {3, 0}}; // cumulative: 1, 2, 3
        size_t n = navGeoTruncateToBudget({0, 0}, waypoints, 2.0f);
        expectTrue(n == 2, "navGeoTruncateToBudget: budget exactly on a cumulative boundary keeps that waypoint");
    }

    // -- navGeoTruncateToBudget: empty waypoints -> 0, no crash ---------------
    {
        std::vector<Point> waypoints;
        size_t n = navGeoTruncateToBudget({0, 0}, waypoints, 10.0f);
        expectTrue(n == 0, "navGeoTruncateToBudget: empty waypoint list returns 0");
    }

    // -- navGeoTruncateToBudget: single-waypoint route, within budget ---------
    {
        std::vector<Point> waypoints = {{1, 0}};
        size_t n = navGeoTruncateToBudget({0, 0}, waypoints, 5.0f);
        expectTrue(n == 1, "navGeoTruncateToBudget: single waypoint within budget -> kept");
    }

    // -- navGeoTruncateToBudget: single-waypoint route, beyond budget ---------
    {
        std::vector<Point> waypoints = {{10, 0}};
        size_t n = navGeoTruncateToBudget({0, 0}, waypoints, 5.0f);
        expectTrue(n == 0, "navGeoTruncateToBudget: single waypoint beyond budget -> 0");
    }

    // -- navGeoTruncateToBudget: from == waypoints[0] (zero-length approach ---
    // leg, the TV-Rummet case) -> behaves as if from weren't prepended at all --
    {
        std::vector<Point> waypoints = {{0, 0}, {1, 0}, {2, 0}}; // from == waypoints[0]
        size_t n = navGeoTruncateToBudget({0, 0}, waypoints, 1.0f);
        expectTrue(n == 2, "navGeoTruncateToBudget: zero-length approach leg is a no-op on the budget");
    }

    // -- navGeoDownsampleTrail: short recording, well under cap — identical to --
    // plain distance-based downsampling (preserves existing behavior) -----------
    {
        std::vector<Point> raw = {{0, 0}, {1, 0}, {2, 0}, {3, 0}}; // 1m apart, all >= 0.3m threshold
        std::vector<Point> kept = navGeoDownsampleTrail(raw, 0.3f, 300);
        expectTrue(kept.size() == 4, "navGeoDownsampleTrail: short recording keeps every point (count)");
        expectTrue(kept[0].x == 0 && kept[3].x == 3, "navGeoDownsampleTrail: short recording spans start to end");
    }

    // -- navGeoDownsampleTrail: long trail, cap forces adaptive threshold growth -
    // so the retained set spans the WHOLE trail rather than stopping partway -----
    {
        std::vector<Point> raw;
        for (int i = 0; i <= 1000; i++)
            raw.push_back({static_cast<float>(i) * 0.01f, 0}); // 1001 points, 0.01m apart, 10m total
        std::vector<Point> kept = navGeoDownsampleTrail(raw, 0.3f, 10);
        expectTrue(kept.size() <= 10, "navGeoDownsampleTrail: long trail respects the hard cap");
        expectTrue(kept.back().x > 9.0f, "navGeoDownsampleTrail: long trail's last waypoint is near the trail's "
                                          "true end, not stuck partway through");
    }

    // -- navGeoDownsampleTrail: empty raw trail -> empty, no crash --------------
    {
        std::vector<Point> raw;
        std::vector<Point> kept = navGeoDownsampleTrail(raw, 0.3f, 300);
        expectTrue(kept.empty(), "navGeoDownsampleTrail: empty raw trail returns empty");
    }

    // -- navGeoDownsampleTrail: single-point raw trail --------------------------
    {
        std::vector<Point> raw = {{5, 5}};
        std::vector<Point> kept = navGeoDownsampleTrail(raw, 0.3f, 300);
        expectTrue(kept.size() == 1 && kept[0].x == 5 && kept[0].y == 5,
                   "navGeoDownsampleTrail: single-point raw trail returns that point");
    }

    // -- navGeoDownsampleTrail: robot stationary the whole time -> one point ---
    {
        std::vector<Point> raw(50, Point{1, 1});
        std::vector<Point> kept = navGeoDownsampleTrail(raw, 0.3f, 300);
        expectTrue(kept.size() == 1, "navGeoDownsampleTrail: stationary trail collapses to a single waypoint");
    }

    // -- navGeoDownsampleTrail: last raw point always kept, even off-threshold --
    {
        std::vector<Point> raw = {{0, 0}, {1, 0}, {1.05f, 0}}; // last point is only 0.05m from the previous kept
        std::vector<Point> kept = navGeoDownsampleTrail(raw, 0.3f, 300);
        expectTrue(kept.size() == 3, "navGeoDownsampleTrail: trail's true final pose is force-kept below threshold");
        expectTrue(kept.back().x > 1.04f && kept.back().x < 1.06f,
                   "navGeoDownsampleTrail: force-kept point is the real last pose, not the last threshold-kept one");
    }

    if (failures > 0) {
        std::printf("\n%d check(s) FAILED\n", failures);
        return 1;
    }
    std::printf("\nAll checks passed\n");
    return 0;
}
