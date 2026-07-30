#ifndef GEOMETRY_H
#define GEOMETRY_H

#include <cmath>
#include <cstddef>
#include <vector>

// Pure geometry primitives for no-go-line crossing/zone membership during path replay.
// Header-only, zero Arduino dependency, host-testable (geometry_selftest.cpp).
// Straight C++ port of frontend/src/views/history/geometry.ts — keep the two in lockstep so
// the editor's live preview and the firmware's replay-time enforcement agree.

struct Point {
    float x = 0.0f;
    float y = 0.0f;

    constexpr Point() = default;
    constexpr Point(float x_, float y_) : x(x_), y(y_) {}
};

// 2D cross product of (b-a) x (c-a). Sign gives orientation of a->b->c.
inline float geomCross(const Point& a, const Point& b, const Point& c) {
    return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
}

// True when point q lies on segment a-b, assuming the three are collinear.
inline bool geomOnSegment(const Point& a, const Point& b, const Point& q) {
    float minX = a.x < b.x ? a.x : b.x;
    float maxX = a.x > b.x ? a.x : b.x;
    float minY = a.y < b.y ? a.y : b.y;
    float maxY = a.y > b.y ? a.y : b.y;
    return minX <= q.x && q.x <= maxX && minY <= q.y && q.y <= maxY;
}

// Standard orientation-based segment intersection (proper crossings plus the
// collinear/touching edge cases). Mirrors geometry.ts::segmentsIntersect.
inline bool segmentsIntersect(const Point& a1, const Point& a2, const Point& b1, const Point& b2) {
    float d1 = geomCross(b1, b2, a1);
    float d2 = geomCross(b1, b2, a2);
    float d3 = geomCross(a1, a2, b1);
    float d4 = geomCross(a1, a2, b2);

    if (((d1 > 0 && d2 < 0) || (d1 < 0 && d2 > 0)) && ((d3 > 0 && d4 < 0) || (d3 < 0 && d4 > 0))) {
        return true;
    }
    // Collinear / endpoint-touching cases.
    if (d1 == 0 && geomOnSegment(b1, b2, a1))
        return true;
    if (d2 == 0 && geomOnSegment(b1, b2, a2))
        return true;
    if (d3 == 0 && geomOnSegment(a1, a2, b1))
        return true;
    if (d4 == 0 && geomOnSegment(a1, a2, b2))
        return true;
    return false;
}

// True when a polyline (open no-go line) crosses the segment a-b anywhere.
// Mirrors geometry.ts::polylineCrossesSegment.
inline bool polylineCrossesSegment(const std::vector<Point>& polyline, const Point& a, const Point& b) {
    for (size_t i = 0; i + 1 < polyline.size(); i++) {
        if (segmentsIntersect(a, b, polyline[i], polyline[i + 1]))
            return true;
    }
    return false;
}

// Ray-casting point-in-polygon (zone filtering). Polygon is an implicitly
// closed ring of vertices; a point exactly on an edge is not specially
// handled (acceptable for zone membership). Mirrors geometry.ts::pointInPolygon.
inline bool pointInPolygon(const Point& pt, const std::vector<Point>& polygon) {
    bool inside = false;
    size_t n = polygon.size();
    for (size_t i = 0, j = n - 1; i < n; j = i++) {
        const Point& pi = polygon[i];
        const Point& pj = polygon[j];
        bool intersects = (pi.y > pt.y) != (pj.y > pt.y) && pt.x < (pj.x - pi.x) * (pt.y - pi.y) / (pj.y - pi.y) + pi.x;
        if (intersects)
            inside = !inside;
    }
    return inside;
}

// Shortest distance from point p to segment a-b. Mirrors geometry.ts::distPointToSegment.
inline float distPointToSegment(const Point& p, const Point& a, const Point& b) {
    float abx = b.x - a.x;
    float aby = b.y - a.y;
    float lenSq = abx * abx + aby * aby;
    if (lenSq == 0.0f) {
        float dx = p.x - a.x;
        float dy = p.y - a.y;
        return sqrtf(dx * dx + dy * dy);
    }
    float t = ((p.x - a.x) * abx + (p.y - a.y) * aby) / lenSq;
    if (t < 0.0f)
        t = 0.0f;
    if (t > 1.0f)
        t = 1.0f;
    float dx = p.x - (a.x + t * abx);
    float dy = p.y - (a.y + t * aby);
    return sqrtf(dx * dx + dy * dy);
}

#endif // GEOMETRY_H
