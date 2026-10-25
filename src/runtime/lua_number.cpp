// A string's number, as Lua 5.4 reads one (lobject.c's luaO_str2num): the
// conversion behind tonumber, arithmetic on strings and every string
// argument a number is wanted for.

#include "runtime/runtime_internal.h"

#include <cctype>
#include <cstdlib>
#include <cstring>
#include <string>

namespace brasslua {

namespace {

bool isLuaSpace(unsigned char c) noexcept {
    return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r';
}

int hexValue(unsigned char c) noexcept {
    if (c >= '0' && c <= '9') return c - '0';
    return (std::tolower(c) - 'a') + 10;
}

// l_str2int: a decimal integer that fits, or a hex one (wrapping around).
bool strToInt(const char* s, const char* end, int64_t& out) noexcept {
    constexpr uint64_t kMaxBy10 = static_cast<uint64_t>(INT64_MAX) / 10;
    constexpr int kMaxLastDigit = static_cast<int>(INT64_MAX % 10);
    uint64_t a = 0;
    bool empty = true;
    while (s < end && isLuaSpace(static_cast<unsigned char>(*s))) ++s;
    bool neg = false;
    if (s < end && *s == '-') {
        neg = true;
        ++s;
    } else if (s < end && *s == '+') {
        ++s;
    }
    if (end - s >= 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        s += 2;
        for (; s < end && std::isxdigit(static_cast<unsigned char>(*s)); ++s) {
            a = a * 16 + static_cast<uint64_t>(hexValue(static_cast<unsigned char>(*s)));
            empty = false;
        }
    } else {
        for (; s < end && std::isdigit(static_cast<unsigned char>(*s)); ++s) {
            int d = *s - '0';
            if (a >= kMaxBy10 && (a > kMaxBy10 || d > kMaxLastDigit + (neg ? 1 : 0))) {
                return false; // overflows: read as a float
            }
            a = a * 10 + static_cast<uint64_t>(d);
            empty = false;
        }
    }
    while (s < end && isLuaSpace(static_cast<unsigned char>(*s))) ++s;
    if (empty || s != end) return false;
    out = static_cast<int64_t>(neg ? 0u - a : a);
    return true;
}

// l_str2d: strtod's reading of the whole string, refusing 'inf' and 'nan'.
bool strToDouble(const char* s, size_t len, double& out) noexcept {
    for (size_t i = 0; i < len; ++i) {
        const char c = s[i];
        if (c == 'n' || c == 'N') return false;
        if (c == '\0') return false; // strtod would stop there: not the whole string
    }
    char* endptr = nullptr;
    out = std::strtod(s, &endptr);
    if (endptr == s) return false;
    while (endptr < s + len && isLuaSpace(static_cast<unsigned char>(*endptr))) ++endptr;
    return endptr == s + len;
}

} // namespace

bool stringToNumber(std::string_view sv, Value& out) noexcept {
    int64_t i = 0;
    if (strToInt(sv.data(), sv.data() + sv.size(), i)) {
        out = Value::fromInt64(i);
        return true;
    }
    // strtod reads to a NUL: copy what is not known to end in one.
    char small[128];
    const char* s = sv.data();
    std::string big;
    if (sv.size() < sizeof(small)) {
        std::memcpy(small, sv.data(), sv.size());
        small[sv.size()] = '\0';
        s = small;
    } else {
        try {
            big.assign(sv);
        } catch (...) {
            return false;
        }
        s = big.c_str();
    }
    double d = 0.0;
    if (strToDouble(s, sv.size(), d)) {
        out = Value::fromDouble(d);
        return true;
    }
    return false;
}

bool stringToIntegerInBase(std::string_view sv, int base, int64_t& out) noexcept {
    const char* s = sv.data();
    const char* end = s + sv.size();
    uint64_t n = 0;
    while (s < end && isLuaSpace(static_cast<unsigned char>(*s))) ++s;
    bool neg = false;
    if (s < end && *s == '-') {
        neg = true;
        ++s;
    }
    if (s >= end || !std::isalnum(static_cast<unsigned char>(*s))) return false;
    do {
        const unsigned char c = static_cast<unsigned char>(*s);
        const int digit = std::isdigit(c) ? c - '0' : (std::toupper(c) - 'A') + 10;
        if (digit >= base) return false;
        n = n * static_cast<uint64_t>(base) + static_cast<uint64_t>(digit);
        ++s;
    } while (s < end && std::isalnum(static_cast<unsigned char>(*s)));
    while (s < end && isLuaSpace(static_cast<unsigned char>(*s))) ++s;
    if (s != end) return false;
    out = static_cast<int64_t>(neg ? 0u - n : n);
    return true;
}

} // namespace brasslua
