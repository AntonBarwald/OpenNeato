#include "point_array_json.h"
// Pull the cap from config.h in the real build; host tests define it via
// -DNAV_MAX_WAYPOINTS so they don't need config.h's chip/pin -D flags.
#ifndef NAV_MAX_WAYPOINTS
#include "config.h"
#endif

// -- JSON scanning helpers (mirrors json_fields.cpp style) -------------------

static int skipWs(const String& s, int pos) {
    int len = static_cast<int>(s.length());
    while (pos < len) {
        char c = s.charAt(pos);
        if (c != ' ' && c != '\t' && c != '\r' && c != '\n')
            break;
        pos++;
    }
    return pos;
}

// Parse a JSON number starting at pos. Returns position after the number, or
// -1 if no valid number is found at pos.
static int parseNumber(const String& s, int pos, float& out) {
    int len = static_cast<int>(s.length());
    int start = pos;
    while (pos < len) {
        char c = s.charAt(pos);
        if ((c >= '0' && c <= '9') || c == '.' || c == '-' || c == '+' || c == 'e' || c == 'E') {
            pos++;
            continue;
        }
        break;
    }
    if (pos == start)
        return -1;
    out = s.substring(start, pos).toFloat();
    return pos;
}

// Parse a quoted JSON key (no escape handling needed — keys here are always
// simple ASCII identifiers: "x", "y", "t"). Returns position after the
// closing quote, or -1 on error.
static int parseKey(const String& s, int pos, String& key) {
    int len = static_cast<int>(s.length());
    if (pos >= len || s.charAt(pos) != '"')
        return -1;
    pos++;
    key = "";
    while (pos < len && s.charAt(pos) != '"') {
        key += s.charAt(pos);
        pos++;
    }
    if (pos >= len)
        return -1; // unterminated string
    return pos + 1;
}

// Parse one point object "{...}" starting at pos (pointing at the opening
// brace). Returns position after the closing brace, or -1 on error (including
// a well-formed object missing x or y).
static int parsePointObject(const String& s, int pos, NavPoint& point) {
    int len = static_cast<int>(s.length());
    pos = skipWs(s, pos);
    if (pos >= len || s.charAt(pos) != '{')
        return -1;
    pos++;

    point = NavPoint();
    bool hasX = false;
    bool hasY = false;

    while (true) {
        pos = skipWs(s, pos);
        if (pos >= len)
            return -1;

        if (s.charAt(pos) == '}') {
            pos++;
            break;
        }
        if (s.charAt(pos) == ',') {
            pos++;
            continue;
        }

        String key;
        pos = parseKey(s, pos, key);
        if (pos < 0)
            return -1;

        pos = skipWs(s, pos);
        if (pos >= len || s.charAt(pos) != ':')
            return -1;
        pos++;
        pos = skipWs(s, pos);

        float value = 0.0f;
        pos = parseNumber(s, pos, value);
        if (pos < 0)
            return -1;

        if (key == "x") {
            point.pt.x = value;
            hasX = true;
        } else if (key == "y") {
            point.pt.y = value;
            hasY = true;
        } else if (key == "t") {
            point.t = value;
            point.hasT = true;
        }
        // Unknown keys are ignored (forward-compatible with extra fields).
    }

    return (hasX && hasY) ? pos : -1;
}

// -- Public API ----------------------------------------------------------------

bool parsePointArray(const String& json, std::vector<NavPoint>& out) {
    out.clear();
    int len = static_cast<int>(json.length());
    int pos = skipWs(json, 0);
    if (pos >= len || json.charAt(pos) != '[')
        return false;
    pos++;

    while (true) {
        pos = skipWs(json, pos);
        if (pos >= len)
            return false; // unterminated array

        if (json.charAt(pos) == ']') {
            pos++;
            break;
        }
        if (json.charAt(pos) == ',') {
            pos++;
            continue;
        }

        NavPoint point;
        pos = parsePointObject(json, pos, point);
        if (pos < 0)
            return false;
        out.push_back(point);
        // Cap element count during parse (not after) so adversarial input can't
        // exhaust the heap before the caller checks the size.
        if (out.size() > NAV_MAX_WAYPOINTS) {
            out.clear();
            return false;
        }
    }

    return true;
}

String serializePointArray(const std::vector<NavPoint>& points) {
    String json = "[";
    for (size_t i = 0; i < points.size(); i++) {
        if (i > 0)
            json += ",";
        const NavPoint& p = points[i];
        json += "{\"x\":" + String(p.pt.x, 3) + ",\"y\":" + String(p.pt.y, 3);
        if (p.hasT)
            json += ",\"t\":" + String(p.t, 1);
        json += "}";
    }
    json += "]";
    return json;
}
