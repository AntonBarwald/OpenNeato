#ifndef POINT_ARRAY_JSON_H
#define POINT_ARRAY_JSON_H

#include <Arduino.h>
#include <vector>
#include "geometry.h"

// Hand-rolled parser/serializer for a JSON array of flat point objects, e.g.
//   [{"x":1.2,"y":3.4,"t":90},{"x":2,"y":2}]
// Zero-dependency, used by /api/navigate waypoints and no-go zone geometry storage — both
// need a flat array of points rather than json_fields' flat object shape.

// One point in a waypoint/geometry array. `t` (heading in degrees) is
// optional in the source JSON; `hasT` records whether it was present so
// serialization can omit it again for points that never had one.
struct NavPoint {
    Point pt;
    float t = 0.0f;
    bool hasT = false;
};

// Parses into `out` (cleared first). True on success (including empty "[]"); false on
// malformed JSON — `out` should not be relied upon in that case.
bool parsePointArray(const String& json, std::vector<NavPoint>& out);

// Serialize points back to a JSON array string. Omits "t" for points where
// hasT is false.
String serializePointArray(const std::vector<NavPoint>& points);

#endif // POINT_ARRAY_JSON_H
