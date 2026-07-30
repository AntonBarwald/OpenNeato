// Host-only self-test for firmware/src/geometry.h — kept out of PlatformIO's src_dir,
// compiles with plain clang++ (zero Arduino dependency). Mirrors what a TS unit test for
// frontend/src/views/history/geometry.ts would exercise; keep in sync with the JS side.
//
// Run:
//   clang++ -std=c++17 -I firmware/src firmware/test_host/geometry_selftest.cpp -o /tmp/geomtest && /tmp/geomtest

#include "geometry.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>

static int failures = 0;

static void expectTrue(bool cond, const char *name) {
    if (!cond) {
        std::printf("FAIL: %s\n", name);
        failures++;
    } else {
        std::printf("PASS: %s\n", name);
    }
}

static bool approxEq(float a, float b, float eps = 1e-4f) {
    return std::fabs(a - b) <= eps;
}

int main() {
    // -- segmentsIntersect: a proper crossing (X shape) ----------------------
    {
        Point a1{0, 0}, a2{4, 4};
        Point b1{0, 4}, b2{4, 0};
        expectTrue(segmentsIntersect(a1, a2, b1, b2), "segmentsIntersect: proper crossing");
    }

    // -- segmentsIntersect: a clear non-crossing (parallel, disjoint) -------
    {
        Point a1{0, 0}, a2{1, 0};
        Point b1{0, 1}, b2{1, 1};
        expectTrue(!segmentsIntersect(a1, a2, b1, b2), "segmentsIntersect: parallel non-crossing");
    }

    // -- segmentsIntersect: collinear touching at a shared endpoint ---------
    {
        Point a1{0, 0}, a2{2, 0};
        Point b1{2, 0}, b2{4, 0};
        expectTrue(segmentsIntersect(a1, a2, b1, b2), "segmentsIntersect: collinear endpoint touch");
    }

    // -- segmentsIntersect: collinear but non-overlapping (gap) -------------
    {
        Point a1{0, 0}, a2{1, 0};
        Point b1{2, 0}, b2{3, 0};
        expectTrue(!segmentsIntersect(a1, a2, b1, b2), "segmentsIntersect: collinear with gap");
    }

    // -- polylineCrossesSegment: an L-shaped polyline crossing a segment ----
    {
        std::vector<Point> polyline = {{0, 2}, {2, 2}, {2, 4}};
        Point a{1, 0}, b{1, 4}; // vertical segment crosses the horizontal leg at (1,2)
        expectTrue(polylineCrossesSegment(polyline, a, b), "polylineCrossesSegment: crosses horizontal leg");

        Point c{5, 0}, d{5, 4}; // far away, no crossing
        expectTrue(!polylineCrossesSegment(polyline, c, d), "polylineCrossesSegment: no crossing");
    }

    // -- pointInPolygon: square (0,0)-(4,0)-(4,4)-(0,4) ----------------------
    {
        std::vector<Point> square = {{0, 0}, {4, 0}, {4, 4}, {0, 4}};
        expectTrue(pointInPolygon({2, 2}, square), "pointInPolygon: center is inside");
        expectTrue(!pointInPolygon({5, 5}, square), "pointInPolygon: outside point");
        expectTrue(!pointInPolygon({-1, 2}, square), "pointInPolygon: outside to the left");
    }

    // -- segmentsIntersect: T-junction (endpoint touches interior, non-collinear) --
    {
        Point a1{0, 0}, a2{4, 0};
        Point b1{2, 0}, b2{2, 3}; // b1 lands mid-way along a1-a2, b runs off perpendicular
        expectTrue(segmentsIntersect(a1, a2, b1, b2), "segmentsIntersect: T-junction endpoint on other segment");
    }

    // -- segmentsIntersect: zero-length segment exactly on another segment --
    {
        Point a1{2, 0}, a2{2, 0}; // degenerate point segment
        Point b1{0, 0}, b2{4, 0};
        expectTrue(segmentsIntersect(a1, a2, b1, b2), "segmentsIntersect: zero-length segment lying on another");
    }

    // -- segmentsIntersect: zero-length segment off another segment ---------
    {
        Point a1{2, 1}, a2{2, 1}; // degenerate point segment, not on b
        Point b1{0, 0}, b2{4, 0};
        expectTrue(!segmentsIntersect(a1, a2, b1, b2), "segmentsIntersect: zero-length segment off another");
    }

    // -- segmentsIntersect: both segments zero-length at the same point -----
    {
        Point a1{2, 2}, a2{2, 2};
        Point b1{2, 2}, b2{2, 2};
        expectTrue(segmentsIntersect(a1, a2, b1, b2), "segmentsIntersect: both zero-length at same point");
    }

    // -- segmentsIntersect: collinear, one segment fully contained in other -
    {
        Point a1{0, 0}, a2{3, 0};
        Point b1{1, 0}, b2{2, 0}; // b lies entirely within a, not just touching
        expectTrue(segmentsIntersect(a1, a2, b1, b2), "segmentsIntersect: collinear full containment");
    }

    // -- pointInPolygon: point exactly on a vertex ---------------------------
    // Ray-casting is asymmetric at boundaries by design (see geometry.h); pins down actual behavior.
    {
        std::vector<Point> square = {{0, 0}, {4, 0}, {4, 4}, {0, 4}};
        expectTrue(pointInPolygon({0, 0}, square), "pointInPolygon: bottom-left vertex reads inside");
        expectTrue(!pointInPolygon({4, 4}, square), "pointInPolygon: top-right vertex reads outside");
    }

    // -- pointInPolygon: point exactly on a polygon edge ---------------------
    {
        std::vector<Point> square = {{0, 0}, {4, 0}, {4, 4}, {0, 4}};
        expectTrue(pointInPolygon({2, 0}, square), "pointInPolygon: bottom edge midpoint reads inside");
        expectTrue(pointInPolygon({0, 2}, square), "pointInPolygon: left edge midpoint reads inside");
        expectTrue(!pointInPolygon({4, 2}, square), "pointInPolygon: right edge midpoint reads outside");
    }

    // -- pointInPolygon: non-convex (L-shaped / notched) polygon ------------
    {
        // L-shape: (0,0)-(4,0)-(4,2)-(2,2)-(2,4)-(0,4). The notch is the square
        // (2,2)-(4,2)-(4,4)-(2,4), which is NOT part of the polygon's interior.
        std::vector<Point> lshape = {{0, 0}, {4, 0}, {4, 2}, {2, 2}, {2, 4}, {0, 4}};
        expectTrue(pointInPolygon({1, 1}, lshape), "pointInPolygon: L-shape main body is inside");
        expectTrue(!pointInPolygon({3, 3}, lshape), "pointInPolygon: L-shape notch is outside");
        expectTrue(pointInPolygon({1, 3}, lshape), "pointInPolygon: L-shape vertical arm is inside");
        expectTrue(pointInPolygon({3, 1}, lshape), "pointInPolygon: L-shape horizontal arm is inside");
    }

    // -- polylineCrossesSegment: multi-vertex (3-segment) polyline ----------
    {
        // A "staple" shape: (0,0) up to (0,2), right to (2,2), down to (2,0).
        std::vector<Point> polyline = {{0, 0}, {0, 2}, {2, 2}, {2, 0}};

        Point a{-1, 1}, b{1, 1}; // crosses the first (leftmost vertical) leg at (0,1)
        expectTrue(polylineCrossesSegment(polyline, a, b), "polylineCrossesSegment: crosses first leg of 3-segment polyline");

        Point c{1, -1}, d{1, 3}; // crosses the middle (top horizontal) leg at (1,2)
        expectTrue(polylineCrossesSegment(polyline, c, d), "polylineCrossesSegment: crosses middle leg of 3-segment polyline");

        Point e{3, -1}, f{3, 3}; // stays to the right of every leg
        expectTrue(!polylineCrossesSegment(polyline, e, f), "polylineCrossesSegment: misses all legs of 3-segment polyline");

        Point g{1, -1}, h{1, -2}; // entirely below the polyline's bounding box
        expectTrue(!polylineCrossesSegment(polyline, g, h), "polylineCrossesSegment: segment entirely outside polyline bbox");
    }

    // -- polylineCrossesSegment: degenerate polylines ------------------------
    {
        std::vector<Point> singleVertex = {{0, 0}};
        Point a{-1, -1}, b{1, 1};
        expectTrue(!polylineCrossesSegment(singleVertex, a, b), "polylineCrossesSegment: single-vertex polyline has no segments");

        std::vector<Point> empty;
        expectTrue(!polylineCrossesSegment(empty, a, b), "polylineCrossesSegment: empty polyline has no segments");
    }

    // -- distPointToSegment -------------------------------------------------
    {
        Point a{0, 0}, b{0, 4};
        expectTrue(approxEq(distPointToSegment({0, 2}, a, b), 0.0f), "distPointToSegment: on segment is 0");
        expectTrue(approxEq(distPointToSegment({1, 2}, a, b), 1.0f), "distPointToSegment: perpendicular distance 1");

        Point c{0, 0}, d{1, 0};
        // Closest point clamps to (1,0); distance from (5,5) is sqrt(4^2+5^2) = sqrt(41)
        float expected = std::sqrt(41.0f);
        expectTrue(approxEq(distPointToSegment({5, 5}, c, d), expected), "distPointToSegment: clamped endpoint");

        // Degenerate segment (zero length) falls back to point-to-point distance
        Point e{3, 4};
        expectTrue(approxEq(distPointToSegment({0, 0}, e, e), 5.0f), "distPointToSegment: degenerate segment");
    }

    if (failures > 0) {
        std::printf("\n%d check(s) FAILED\n", failures);
        return 1;
    }
    std::printf("\nAll checks passed\n");
    return 0;
}
