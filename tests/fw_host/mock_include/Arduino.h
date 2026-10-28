// Host stand-in for the Ameba Pro2 Arduino core: just enough of Arduino.h for the extracted firmware code.
// Behaviour copies the Realtek core where the firmware depends on it (e.g. String(float, n) padding).
#pragma once
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

// ---------------------------------------------------------------- fake clock
extern uint32_t g_fake_millis;
inline uint32_t millis() { return g_fake_millis; }
inline void delay(uint32_t ms) { g_fake_millis += ms; }

// ---------------------------------------------------------------- String (Arduino API subset)
class String {
public:
    std::string s;
    String() {}
    String(const char *c) : s(c ? c : "") {}
    String(const std::string &x) : s(x) {}
    String(const String &o) = default;
    String &operator=(const String &o) = default;
    explicit String(char c) : s(1, c) {}
    explicit String(int v) : s(std::to_string(v)) {}
    explicit String(unsigned int v) : s(std::to_string(v)) {}
    explicit String(long v) : s(std::to_string(v)) {}
    explicit String(unsigned long v) : s(std::to_string(v)) {}
    explicit String(long long v) : s(std::to_string(v)) {}
    explicit String(unsigned long long v) : s(std::to_string(v)) {}
    // Realtek core: dtostrf(value, decimals + 2, decimals) -> right-aligned, minimum width decimals + 2
    explicit String(float v, unsigned char dec = 2) { fmt((double)v, dec); }
    explicit String(double v, unsigned char dec = 2) { fmt(v, dec); }

    unsigned int length() const { return (unsigned)s.size(); }
    const char *c_str() const { return s.c_str(); }
    char operator[](unsigned i) const { return i < s.size() ? s[i] : 0; }
    char charAt(unsigned i) const { return (*this)[i]; }
    long toInt() const { return std::strtol(s.c_str(), nullptr, 10); }
    float toFloat() const { return std::strtof(s.c_str(), nullptr); }
    int indexOf(char c, unsigned from = 0) const { auto p = s.find(c, from); return p == std::string::npos ? -1 : (int)p; }
    int indexOf(const char *t, unsigned from = 0) const { auto p = s.find(t, from); return p == std::string::npos ? -1 : (int)p; }
    int indexOf(const String &t, unsigned from = 0) const { return indexOf(t.c_str(), from); }
    String substring(unsigned a) const { return a >= s.size() ? String("") : String(s.substr(a)); }
    String substring(unsigned a, unsigned b) const
    {
        if (a > b) std::swap(a, b);
        if (a >= s.size()) return String("");
        if (b > s.size()) b = (unsigned)s.size();
        return String(s.substr(a, b - a));
    }
    void trim()
    {
        size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
        s = (a == std::string::npos) ? "" : s.substr(a, b - a + 1);
    }
    bool startsWith(const char *p) const { return s.rfind(p, 0) == 0; }
    bool startsWith(const String &p) const { return startsWith(p.c_str()); }
    bool endsWith(const char *p) const { size_t n = strlen(p); return s.size() >= n && s.compare(s.size() - n, n, p) == 0; }
    bool equalsIgnoreCase(const String &o) const
    {
        if (o.s.size() != s.size()) return false;
        for (size_t i = 0; i < s.size(); i++)
            if (tolower((unsigned char)s[i]) != tolower((unsigned char)o.s[i])) return false;
        return true;
    }
    bool equalsIgnoreCase(const char *o) const { return equalsIgnoreCase(String(o)); }
    void toUpperCase() { for (auto &c : s) c = (char)toupper((unsigned char)c); }
    void toLowerCase() { for (auto &c : s) c = (char)tolower((unsigned char)c); }
    bool concat(const String &o) { s += o.s; return true; }
    String &operator+=(const String &o) { s += o.s; return *this; }
    String &operator+=(const char *o) { s += o; return *this; }
    String &operator+=(char c) { s += c; return *this; }
    bool operator==(const String &o) const { return s == o.s; }
    bool operator==(const char *o) const { return s == (o ? o : ""); }
    bool operator!=(const String &o) const { return s != o.s; }
    bool operator!=(const char *o) const { return !(*this == o); }
    operator std::string() const { return s; }

private:
    void fmt(double v, unsigned char dec)
    {
        char buf[64];
        snprintf(buf, sizeof(buf), "%*.*f", dec + 2, dec, v);
        s = buf;
    }
};

inline String operator+(const String &a, const String &b) { return String(a.s + b.s); }
inline String operator+(const String &a, const char *b) { return String(a.s + (b ? b : "")); }
inline String operator+(const char *a, const String &b) { return String(std::string(a ? a : "") + b.s); }
inline String operator+(const String &a, char c) { return String(a.s + c); }
inline String operator+(const String &a, int v) { return a + String(v); }
inline String operator+(const String &a, unsigned int v) { return a + String(v); }
inline String operator+(const String &a, long v) { return a + String(v); }
inline String operator+(const String &a, unsigned long v) { return a + String(v); }
inline bool operator==(const char *a, const String &b) { return b == a; }
#include <ostream>
inline std::ostream &operator<<(std::ostream &o, const String &s) { return o << '"' << s.s << '"'; }

// ---------------------------------------------------------------- Serial
struct FakeSerial {
    std::vector<std::string> lines;
    std::string rx;
    void println(const String &x) { lines.push_back(x.s); }
    void println(const char *x) { lines.push_back(x); }
    void println() { lines.push_back(""); }
    void print(const String &x) { lines.push_back(x.s); }
    void print(const char *x) { lines.push_back(x); }
    int available() { return (int)rx.size(); }
    int read() { if (rx.empty()) return -1; int c = (unsigned char)rx[0]; rx.erase(0, 1); return c; }
    void begin(long) {}
};
extern FakeSerial Serial;
