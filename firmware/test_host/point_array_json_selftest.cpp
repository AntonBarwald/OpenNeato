// Host-only self-test for firmware/src/point_array_json.h/.cpp — compiles against the
// Arduino String shim (put its include path first so <Arduino.h> resolves to it).
// -DNAV_MAX_WAYPOINTS sidesteps config.h's -DCHIP_MODEL requirement and gives the cap test
// a small, fast bound instead of the real firmware's 500.
//
// Run:
//   clang++ -std=c++17 -I firmware/test_host/arduino_shim -I firmware/src -DNAV_MAX_WAYPOINTS=8 \
//       firmware/test_host/point_array_json_selftest.cpp firmware/src/point_array_json.cpp \
//       -o /tmp/patest && /tmp/patest

#include "point_array_json.h"
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

static bool approxEq(float a, float b, float eps = 1e-3f) {
    float d = a - b;
    if (d < 0)
        d = -d;
    return d <= eps;
}

int main() {
    // -- Round trip: two points, one with t, one without --------------------
    {
        String json = "[{\"x\":1.2,\"y\":3.4,\"t\":90},{\"x\":2,\"y\":-2.5}]";
        std::vector<NavPoint> points;
        bool ok = parsePointArray(json, points);
        expectTrue(ok, "parsePointArray: valid array parses ok");
        expectTrue(points.size() == 2, "parsePointArray: correct point count");
        expectTrue(approxEq(points[0].pt.x, 1.2f) && approxEq(points[0].pt.y, 3.4f),
                   "parsePointArray: point 0 x/y");
        expectTrue(points[0].hasT && approxEq(points[0].t, 90.0f), "parsePointArray: point 0 has t=90");
        expectTrue(approxEq(points[1].pt.x, 2.0f) && approxEq(points[1].pt.y, -2.5f),
                   "parsePointArray: point 1 x/y (negative y)");
        expectTrue(!points[1].hasT, "parsePointArray: point 1 has no t");

        String reserialized = serializePointArray(points);
        std::vector<NavPoint> reparsed;
        bool ok2 = parsePointArray(reserialized, reparsed);
        expectTrue(ok2, "round-trip: reserialized JSON reparses ok");
        expectTrue(reparsed.size() == 2, "round-trip: point count preserved");
        expectTrue(approxEq(reparsed[0].pt.x, 1.2f) && approxEq(reparsed[0].pt.y, 3.4f),
                   "round-trip: point 0 x/y preserved");
        expectTrue(reparsed[0].hasT && approxEq(reparsed[0].t, 90.0f), "round-trip: point 0 t preserved");
        expectTrue(!reparsed[1].hasT, "round-trip: point 1 still has no t");
    }

    // -- Empty array ----------------------------------------------------------
    {
        std::vector<NavPoint> points;
        bool ok = parsePointArray("[]", points);
        expectTrue(ok, "parsePointArray: empty array parses ok");
        expectTrue(points.empty(), "parsePointArray: empty array yields no points");
        expectTrue(serializePointArray(points) == "[]", "serializePointArray: empty vector serializes to []");
    }

    // -- Whitespace tolerance ---------------------------------------------------
    {
        std::vector<NavPoint> points;
        bool ok = parsePointArray("  [ { \"x\" : 0 , \"y\" : 0 } ]  ", points);
        expectTrue(ok, "parsePointArray: tolerates surrounding/inner whitespace");
        expectTrue(points.size() == 1, "parsePointArray: whitespace variant point count");
    }

    // -- Malformed input --------------------------------------------------------
    {
        std::vector<NavPoint> points;
        expectTrue(!parsePointArray("", points), "parsePointArray: empty string is invalid");
        expectTrue(!parsePointArray("{\"x\":1,\"y\":2}", points), "parsePointArray: object instead of array is invalid");
        expectTrue(!parsePointArray("[{\"x\":1}]", points), "parsePointArray: missing y is invalid");
        expectTrue(!parsePointArray("[{\"y\":1}]", points), "parsePointArray: missing x is invalid");
        expectTrue(!parsePointArray("[{\"x\":1,\"y\":2}", points), "parsePointArray: unterminated array is invalid");
    }

    // -- Malformed input: more shapes --------------------------------------
    {
        std::vector<NavPoint> points;
        expectTrue(!parsePointArray("[{\"x\":\"a\",\"y\":2}]", points), "parsePointArray: non-numeric x is invalid");
        expectTrue(!parsePointArray("[{\"x\":1,\"y\":\"b\"}]", points), "parsePointArray: non-numeric y is invalid");
        expectTrue(!parsePointArray("[{\"x\"1,\"y\":2}]", points), "parsePointArray: missing colon is invalid");
        expectTrue(!parsePointArray("[1,2,3]", points), "parsePointArray: array of bare numbers (not objects) is invalid");
        expectTrue(!parsePointArray("[[1,2]]", points), "parsePointArray: nested array instead of object is invalid");
        expectTrue(!parsePointArray("not json at all", points), "parsePointArray: non-JSON garbage is invalid");

        // Unlike the NAV_MAX_WAYPOINTS-cap path, this failure path does NOT roll back `out`
        // (header doc already tells callers not to rely on `out` on failure).
        points.clear();
        bool ok = parsePointArray("[{\"x\":1,\"y\":2}", points);
        expectTrue(!ok, "parsePointArray: element-then-truncation is invalid");
    }

    // -- Lenient comma handling (documented finding, not a crash/hang) ------
    // Known-lenient: leading/trailing/doubled commas are accepted, not rejected.
    {
        std::vector<NavPoint> points;

        bool okTrailing = parsePointArray("[{\"x\":1,\"y\":2},]", points);
        expectTrue(okTrailing, "parsePointArray: [KNOWN LENIENT] trailing comma is accepted, not rejected");
        expectTrue(points.size() == 1, "parsePointArray: trailing comma still yields the one real element");

        bool okLeading = parsePointArray("[,]", points);
        expectTrue(okLeading, "parsePointArray: [KNOWN LENIENT] lone leading comma is accepted, not rejected");
        expectTrue(points.empty(), "parsePointArray: lone leading comma yields zero elements");

        bool okDouble = parsePointArray("[{\"x\":1,\"y\":2},,{\"x\":3,\"y\":4}]", points);
        expectTrue(okDouble, "parsePointArray: [KNOWN LENIENT] doubled comma is accepted, not rejected");
        expectTrue(points.size() == 2, "parsePointArray: doubled comma still yields both real elements");

        // Same root cause one level down: parsePointObject never requires a comma between fields.
        bool okMissingFieldComma = parsePointArray("[{\"x\":1\"y\":2}]", points);
        expectTrue(okMissingFieldComma, "parsePointArray: [KNOWN LENIENT] missing comma between fields is accepted, not rejected");
        expectTrue(points.size() == 1 && approxEq(points[0].pt.x, 1.0f) && approxEq(points[0].pt.y, 2.0f),
                   "parsePointArray: missing field comma still parses both x and y");
    }

    // -- Whitespace tolerance: tabs and newlines mixed in --------------------
    {
        std::vector<NavPoint> points;
        String json = "\n\t[\n\t{\"x\":1,\n\t\"y\":2,\n\t\"t\":5}\n\t]\n";
        bool ok = parsePointArray(json, points);
        expectTrue(ok, "parsePointArray: tolerates tabs/newlines around tokens");
        expectTrue(points.size() == 1, "parsePointArray: tab/newline variant point count");
        expectTrue(points[0].hasT && approxEq(points[0].t, 5.0f), "parsePointArray: tab/newline variant keeps t");
    }

    // -- t variants: integer, float, negative, and missing -------------------
    {
        std::vector<NavPoint> points;
        bool ok = parsePointArray(
            "[{\"x\":0,\"y\":0,\"t\":90},{\"x\":0,\"y\":0,\"t\":90.5},{\"x\":0,\"y\":0,\"t\":-45},{\"x\":0,\"y\":0}]",
            points);
        expectTrue(ok, "parsePointArray: t-variants array parses ok");
        expectTrue(points.size() == 4, "parsePointArray: t-variants point count");

        expectTrue(points[0].hasT && approxEq(points[0].t, 90.0f), "parsePointArray: integer t parsed exactly");
        expectTrue(points[1].hasT && approxEq(points[1].t, 90.5f), "parsePointArray: float t parsed exactly");
        expectTrue(points[2].hasT && approxEq(points[2].t, -45.0f), "parsePointArray: negative t parsed exactly");
        expectTrue(!points[3].hasT, "parsePointArray: point with no t key has hasT=false");
        expectTrue(approxEq(points[3].t, 0.0f), "parsePointArray: point with no t key defaults t to 0");
    }

    // -- NAV_MAX_WAYPOINTS cap (this file is compiled with -DNAV_MAX_WAYPOINTS=8) --
    {
        // Written against the macro, not a hardcoded 8/9, so it stays correct if the flag changes.
        String atCap = "[";
        for (int i = 0; i < NAV_MAX_WAYPOINTS; i++) {
            if (i > 0)
                atCap += ",";
            atCap += "{\"x\":0,\"y\":0}";
        }
        String overCap = atCap + ",{\"x\":0,\"y\":0}]";
        atCap += "]";

        std::vector<NavPoint> points;
        bool okAtCap = parsePointArray(atCap, points);
        expectTrue(okAtCap, "parsePointArray: exactly NAV_MAX_WAYPOINTS points is accepted");
        expectTrue(points.size() == static_cast<size_t>(NAV_MAX_WAYPOINTS),
                   "parsePointArray: at-cap point count matches NAV_MAX_WAYPOINTS");

        bool okOverCap = parsePointArray(overCap, points);
        expectTrue(!okOverCap, "parsePointArray: NAV_MAX_WAYPOINTS+1 points is rejected");
        expectTrue(points.empty(), "parsePointArray: over-cap failure clears out (cap path does roll back)");
    }

    if (failures > 0) {
        std::printf("\n%d check(s) FAILED\n", failures);
        return 1;
    }
    std::printf("\nAll checks passed\n");
    return 0;
}
