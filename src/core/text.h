// SPDX-License-Identifier: GPL-2.0-or-later

// String helpers.
//
// std::to_string is not inline. libc++ keeps it in the compiled library, which
// this project does not build for the Xbox 360.

#pragma once

#include <cstdint>
#include <string>

namespace bj {

inline std::string ToString(unsigned long long value)
{
    char buf[24];
    char* at = buf + sizeof(buf);
    *--at = '\0';
    do {
        *--at = (char)('0' + (int)(value % 10u));
        value /= 10u;
    } while (value);
    return at;
}

inline std::string ToString(long long value)
{
    if (value < 0) {
        // Negated as unsigned so the most negative value has somewhere to go.
        return "-" + ToString((unsigned long long)(~(unsigned long long)value + 1u));
    }
    return ToString((unsigned long long)value);
}

// Fixed point, because a double cannot be passed to printf on this ABI either.
// Values beyond what a 64-bit integer can scale are printed whole, which is
// well past anything this program measures.
inline std::string ToString(double value, int decimals)
{
    if (value != value) return "nan";               // the only NaN test that needs no math.h

    const bool negative = value < 0.0;
    if (negative) value = -value;
    if (value > 1.0e15) return negative ? "-big" : "big";

    unsigned long long scale = 1;
    for (int i = 0; i < decimals; ++i) scale *= 10u;

    const unsigned long long scaled = (unsigned long long)(value * (double)scale + 0.5);
    const unsigned long long whole  = scaled / scale;
    const unsigned long long frac   = scaled % scale;

    std::string out = negative ? "-" : "";
    out += ToString(whole);
    if (decimals > 0) {
        std::string digits = ToString(frac);
        while ((int)digits.size() < decimals) digits.insert(digits.begin(), '0');
        out += "." + digits;
    }
    return out;
}

inline std::string ToString(int value)           { return ToString((long long)value); }
inline std::string ToString(long value)          { return ToString((long long)value); }
inline std::string ToString(unsigned value)      { return ToString((unsigned long long)value); }
inline std::string ToString(unsigned long value) { return ToString((unsigned long long)value); }

}  // namespace bj
