// Minimal host-only stand-in for Arduino.h's String API, used only when a host test build
// points -I at this directory ahead of firmware/src. Not used by the real firmware build.
// Keep tiny — add methods only as new host tests need them.
#ifndef ARDUINO_SHIM_H
#define ARDUINO_SHIM_H

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

// Arduino's isDigit(c) is a thin wrapper around isdigit() taking a char
// (rather than the int-with-EOF-sentinel signature of <cctype>'s isdigit).
inline bool isDigit(char c) { return std::isdigit(static_cast<unsigned char>(c)) != 0; }

class String {
public:
    String() = default;
    String(const char *s) : s_(s ? s : "") {}
    String(char c) : s_(1, c) {}
    String(int v) {
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%d", v);
        s_ = buf;
    }
    // Mirrors Arduino's String(float, unsigned char decimalPlaces) constructor.
    String(float v, int decimals) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%.*f", decimals, static_cast<double>(v));
        s_ = buf;
    }
    String(const std::string& s) : s_(s) {}

    size_t length() const { return s_.length(); }
    char charAt(size_t i) const { return s_[i]; }
    const char *c_str() const { return s_.c_str(); }
    bool isEmpty() const { return s_.empty(); }

    String substring(size_t from) const {
        if (from >= s_.length())
            return String("");
        return String(s_.substr(from));
    }
    String substring(size_t from, size_t to) const {
        if (from >= s_.length() || to <= from)
            return String("");
        if (to > s_.length())
            to = s_.length();
        return String(s_.substr(from, to - from));
    }

    float toFloat() const { return static_cast<float>(std::atof(s_.c_str())); }
    long toInt() const { return std::atol(s_.c_str()); }

    bool endsWith(const char *suffix) const {
        std::string suf(suffix);
        if (suf.size() > s_.size())
            return false;
        return s_.compare(s_.size() - suf.size(), suf.size(), suf) == 0;
    }

    bool startsWith(const char *prefix) const {
        std::string pre(prefix);
        if (pre.size() > s_.size())
            return false;
        return s_.compare(0, pre.size(), pre) == 0;
    }

    // Single-char search from the start (Arduino: indexOf(char)).
    int indexOf(char c) const {
        size_t pos = s_.find(c);
        return pos == std::string::npos ? -1 : static_cast<int>(pos);
    }
    // Single-char search starting at fromIndex (Arduino: indexOf(char, unsigned int)).
    int indexOf(char c, size_t fromIndex) const {
        if (fromIndex > s_.size())
            return -1;
        size_t pos = s_.find(c, fromIndex);
        return pos == std::string::npos ? -1 : static_cast<int>(pos);
    }
    // Substring search from the start (Arduino: indexOf(const String&)); also
    // covers plain string-literal callers since const char* converts to String.
    int indexOf(const char *str) const {
        size_t pos = s_.find(str);
        return pos == std::string::npos ? -1 : static_cast<int>(pos);
    }
    // Substring search starting at fromIndex (Arduino: indexOf(const String&, unsigned int)).
    int indexOf(const char *str, size_t fromIndex) const {
        if (fromIndex > s_.size())
            return -1;
        size_t pos = s_.find(str, fromIndex);
        return pos == std::string::npos ? -1 : static_cast<int>(pos);
    }
    int lastIndexOf(char c) const {
        size_t pos = s_.rfind(c);
        return pos == std::string::npos ? -1 : static_cast<int>(pos);
    }

    // In-place, strips leading/trailing whitespace — matches Arduino's own
    // trim(), which treats anything isspace() considers whitespace (space,
    // tab, CR, LF, VT, FF) as trimmable, not just plain spaces.
    void trim() {
        size_t start = 0;
        while (start < s_.size() && std::isspace(static_cast<unsigned char>(s_[start])))
            start++;
        size_t end = s_.size();
        while (end > start && std::isspace(static_cast<unsigned char>(s_[end - 1])))
            end--;
        s_ = s_.substr(start, end - start);
    }

    // In-place ASCII lowercase (Arduino's toLowerCase() is ASCII-only too).
    void toLowerCase() {
        for (auto& ch: s_)
            ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }

    bool equalsIgnoreCase(const char *other) const {
        return strcasecmp(s_.c_str(), other) == 0;
    }
    bool equalsIgnoreCase(const String& other) const { return equalsIgnoreCase(other.s_.c_str()); }

    // In-place, replaces every non-overlapping occurrence (Arduino semantics:
    // scans left to right, resuming search after the replacement text so a
    // replacement containing the search text isn't re-matched).
    void replace(const char *find, const char *repl) {
        std::string f(find), r(repl);
        if (f.empty())
            return;
        std::string result;
        size_t pos = 0;
        while (true) {
            size_t next = s_.find(f, pos);
            if (next == std::string::npos) {
                result += s_.substr(pos);
                break;
            }
            result += s_.substr(pos, next - pos);
            result += r;
            pos = next + f.size();
        }
        s_ = result;
    }

    // Read-only element access (Arduino's non-const operator[] returns a
    // mutable reference; every caller in this codebase only reads a
    // character, so the const-returning form is sufficient here).
    char operator[](size_t index) const { return s_[index]; }

    void reserve(size_t n) { s_.reserve(n); }

    String& operator+=(const String& other) {
        s_ += other.s_;
        return *this;
    }
    String& operator+=(char c) {
        s_ += c;
        return *this;
    }
    String& operator+=(const char *cstr) {
        s_ += cstr;
        return *this;
    }

    bool operator==(const char *other) const { return s_ == other; }
    bool operator==(const String& other) const { return s_ == other.s_; }
    bool operator!=(const String& other) const { return s_ != other.s_; }

    friend String operator+(String lhs, const String& rhs) {
        lhs += rhs;
        return lhs;
    }
    friend String operator+(String lhs, const char *rhs) {
        lhs += rhs;
        return lhs;
    }
    friend String operator+(const char *lhs, const String& rhs) {
        String tmp(lhs);
        tmp += rhs;
        return tmp;
    }

private:
    std::string s_;
};

#endif // ARDUINO_SHIM_H
