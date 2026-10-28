// Host stand-in for the Ameba IPAddress class (network-order uint32 conversion, byte access).
#pragma once
#include <cstdint>
#include "Arduino.h"
class IPAddress {
public:
    uint8_t b[4] = {0, 0, 0, 0};
    IPAddress() {}
    IPAddress(uint8_t a, uint8_t c, uint8_t d, uint8_t e) { b[0] = a; b[1] = c; b[2] = d; b[3] = e; }
    operator uint32_t() const { uint32_t v; memcpy(&v, b, 4); return v; }
    uint8_t operator[](int i) const { return b[i]; }
    uint8_t &operator[](int i) { return b[i]; }
    String toStr() const { return String((int)b[0]) + "." + String((int)b[1]) + "." + String((int)b[2]) + "." + String((int)b[3]); }
};
