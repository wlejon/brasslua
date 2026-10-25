#include "runtime/runtime_internal.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <limits>
#include <random>

namespace brasslua {

namespace {

std::mt19937_64& rngEngine() {
    static std::mt19937_64 rng(0x123456789ABCDEF0ULL);
    return rng;
}

double argDouble(const uint64_t* args, int64_t nargs, int64_t idx) {
    if (idx >= nargs) return 0.0;
    double d = 0.0;
    if (tryCoerceToDouble(Value(args[idx]), d)) return d;
    throw LuaRuntimeError("bad argument to math function (number expected)");
}

int32_t argBit32(const uint64_t* args, int64_t nargs, int64_t idx) {
    if (idx >= nargs) return 0;
    Value v(args[idx]);
    if (v.isInteger()) {
        return static_cast<int32_t>(static_cast<uint32_t>(static_cast<uint64_t>(v.asInt64())));
    }
    double d = 0.0;
    if (tryCoerceToDouble(v, d)) {
        int64_t iv = static_cast<int64_t>(d);
        return static_cast<int32_t>(static_cast<uint32_t>(static_cast<uint64_t>(iv)));
    }
    throw LuaRuntimeError("bad argument to bit function (number expected)");
}

uint64_t mathAbs(void* /*cl*/, const uint64_t* args, int64_t nargs) {
    if (nargs >= 1 && Value(args[0]).isInteger()) {
        int64_t i = Value(args[0]).asInt64();
        if (i < 0) {
            i = static_cast<int64_t>(0ULL - static_cast<uint64_t>(i));
        }
        return Value::fromInt64(i).rawBits();
    }
    return Value::fromDouble(std::fabs(argDouble(args, nargs, 0))).rawBits();
}

uint64_t mathFloor(void* /*cl*/, const uint64_t* args, int64_t nargs) {
    if (nargs >= 1 && Value(args[0]).isInteger()) {
        return args[0];
    }
    double f = std::floor(argDouble(args, nargs, 0));
    int64_t iv = 0;
    if (tryConvertFloatToExactInt64(f, iv)) {
        return Value::fromInt64(iv).rawBits();
    }
    return Value::fromDouble(f).rawBits();
}

uint64_t mathCeil(void* /*cl*/, const uint64_t* args, int64_t nargs) {
    if (nargs >= 1 && Value(args[0]).isInteger()) {
        return args[0];
    }
    double c = std::ceil(argDouble(args, nargs, 0));
    int64_t iv = 0;
    if (tryConvertFloatToExactInt64(c, iv)) {
        return Value::fromInt64(iv).rawBits();
    }
    return Value::fromDouble(c).rawBits();
}

uint64_t mathSqrt(void* /*cl*/, const uint64_t* args, int64_t nargs) {
    return Value::fromDouble(std::sqrt(argDouble(args, nargs, 0))).rawBits();
}

uint64_t mathSin(void* /*cl*/, const uint64_t* args, int64_t nargs) {
    return Value::fromDouble(std::sin(argDouble(args, nargs, 0))).rawBits();
}

uint64_t mathCos(void* /*cl*/, const uint64_t* args, int64_t nargs) {
    return Value::fromDouble(std::cos(argDouble(args, nargs, 0))).rawBits();
}

uint64_t mathTan(void* /*cl*/, const uint64_t* args, int64_t nargs) {
    return Value::fromDouble(std::tan(argDouble(args, nargs, 0))).rawBits();
}

uint64_t mathAsin(void* /*cl*/, const uint64_t* args, int64_t nargs) {
    return Value::fromDouble(std::asin(argDouble(args, nargs, 0))).rawBits();
}

uint64_t mathAcos(void* /*cl*/, const uint64_t* args, int64_t nargs) {
    return Value::fromDouble(std::acos(argDouble(args, nargs, 0))).rawBits();
}

uint64_t mathAtan(void* /*cl*/, const uint64_t* args, int64_t nargs) {
    double y = argDouble(args, nargs, 0);
    if (nargs >= 2 && !Value(args[1]).isNil()) {
        double x = argDouble(args, nargs, 1);
        return Value::fromDouble(std::atan2(y, x)).rawBits();
    }
    return Value::fromDouble(std::atan(y)).rawBits();
}

uint64_t mathExp(void* /*cl*/, const uint64_t* args, int64_t nargs) {
    return Value::fromDouble(std::exp(argDouble(args, nargs, 0))).rawBits();
}

uint64_t mathLog(void* /*cl*/, const uint64_t* args, int64_t nargs) {
    double x = argDouble(args, nargs, 0);
    if (nargs >= 2 && !Value(args[1]).isNil()) {
        double base = argDouble(args, nargs, 1);
        return Value::fromDouble(std::log(x) / std::log(base)).rawBits();
    }
    return Value::fromDouble(std::log(x)).rawBits();
}

uint64_t mathLog10(void* /*cl*/, const uint64_t* args, int64_t nargs) {
    return Value::fromDouble(std::log10(argDouble(args, nargs, 0))).rawBits();
}

uint64_t mathFmod(void* /*cl*/, const uint64_t* args, int64_t nargs) {
    if (nargs >= 2 && Value(args[0]).isInteger() && Value(args[1]).isInteger()) {
        int64_t a = Value(args[0]).asInt64();
        int64_t b = Value(args[1]).asInt64();
        if (b == 0) throw LuaRuntimeError("bad argument #2 to 'fmod' (zero)");
        if (a == std::numeric_limits<int64_t>::min() && b == -1) {
            return Value::fromInt64(0).rawBits();
        }
        return Value::fromInt64(a % b).rawBits();
    }
    return Value::fromDouble(std::fmod(argDouble(args, nargs, 0), argDouble(args, nargs, 1))).rawBits();
}

uint64_t mathModf(void* /*cl*/, const uint64_t* args, int64_t nargs) {
    auto& rt = RuntimeState::current();
    if (nargs >= 1 && Value(args[0]).isInteger()) {
        return returnMulti(rt, {args[0], Value::fromDouble(0.0).rawBits()});
    }
    double x = argDouble(args, nargs, 0);
    double int_part = 0.0;
    double frac_part = std::modf(x, &int_part);
    int64_t iv = 0;
    uint64_t first = tryConvertFloatToExactInt64(int_part, iv)
                         ? Value::fromInt64(iv).rawBits()
                         : Value::fromDouble(int_part).rawBits();
    return returnMulti(rt, {first, Value::fromDouble(frac_part).rawBits()});
}

uint64_t mathDeg(void* /*cl*/, const uint64_t* args, int64_t nargs) {
    constexpr double kRadToDeg = 180.0 / 3.14159265358979323846;
    return Value::fromDouble(argDouble(args, nargs, 0) * kRadToDeg).rawBits();
}

uint64_t mathRad(void* /*cl*/, const uint64_t* args, int64_t nargs) {
    constexpr double kDegToRad = 3.14159265358979323846 / 180.0;
    return Value::fromDouble(argDouble(args, nargs, 0) * kDegToRad).rawBits();
}

uint64_t mathMin(void* /*cl*/, const uint64_t* args, int64_t nargs) {
    if (nargs < 1) throw LuaRuntimeError("bad argument #1 to 'min' (value expected)");
    uint64_t best = args[0];
    for (int64_t i = 1; i < nargs; ++i) {
        if (brasslua_rt_lt(args[i], best)) {
            best = args[i];
        }
    }
    return best;
}

uint64_t mathMax(void* /*cl*/, const uint64_t* args, int64_t nargs) {
    if (nargs < 1) throw LuaRuntimeError("bad argument #1 to 'max' (value expected)");
    uint64_t best = args[0];
    for (int64_t i = 1; i < nargs; ++i) {
        if (brasslua_rt_lt(best, args[i])) {
            best = args[i];
        }
    }
    return best;
}

uint64_t mathRandom(void* /*cl*/, const uint64_t* args, int64_t nargs) {
    auto& rng = rngEngine();
    if (nargs == 0) {
        std::uniform_real_distribution<double> dist(0.0, 1.0);
        return Value::fromDouble(dist(rng)).rawBits();
    }
    if (nargs == 1) {
        int64_t hi = 0;
        if (!tryCoerceToInt64(Value(args[0]), hi)) {
            throw LuaRuntimeError("number has no integer representation");
        }
        if (hi == 0) {
            return Value::fromInt64(static_cast<int64_t>(rng())).rawBits();
        }
        if (hi < 1) throw LuaRuntimeError("bad argument #1 to 'random' (interval is empty)");
        std::uniform_int_distribution<int64_t> dist(1, hi);
        return Value::fromInt64(dist(rng)).rawBits();
    }
    int64_t lo = 0, hi = 0;
    if (!tryCoerceToInt64(Value(args[0]), lo) || !tryCoerceToInt64(Value(args[1]), hi)) {
        throw LuaRuntimeError("number has no integer representation");
    }
    if (hi < lo) throw LuaRuntimeError("bad argument #2 to 'random' (interval is empty)");
    std::uniform_int_distribution<int64_t> dist(lo, hi);
    return Value::fromInt64(dist(rng)).rawBits();
}

uint64_t mathRandomseed(void* /*cl*/, const uint64_t* args, int64_t nargs) {
    uint64_t seed = nargs > 0 ? static_cast<uint64_t>(Value(args[0]).asInt64()) : 1ULL;
    rngEngine().seed(seed);
    return kNilBits;
}

uint64_t mathType(void* /*cl*/, const uint64_t* args, int64_t nargs) {
    auto& rt = RuntimeState::current();
    if (nargs < 1) return kNilBits;
    Value v(args[0]);
    if (v.isInteger()) {
        return rt.makeStringValue("integer");
    }
    if (v.isFloat()) {
        return rt.makeStringValue("float");
    }
    return kNilBits;
}

uint64_t mathTointeger(void* /*cl*/, const uint64_t* args, int64_t nargs) {
    if (nargs < 1) return kNilBits;
    int64_t out = 0;
    if (tryCoerceToInt64(Value(args[0]), out)) {
        return Value::fromInt64(out).rawBits();
    }
    return kNilBits;
}

uint64_t mathUlt(void* /*cl*/, const uint64_t* args, int64_t nargs) {
    if (nargs < 2) throw LuaRuntimeError("bad argument to 'ult' (number expected)");
    int64_t a = 0, b = 0;
    if (!tryCoerceToInt64(Value(args[0]), a) || !tryCoerceToInt64(Value(args[1]), b)) {
        throw LuaRuntimeError("number has no integer representation");
    }
    return Value::fromBool(static_cast<uint64_t>(a) < static_cast<uint64_t>(b)).rawBits();
}

// ---- LuaJIT `bit` Library (32-bit signed two's-complement) ----

uint64_t bitTobit(void* /*cl*/, const uint64_t* args, int64_t nargs) {
    int32_t x = argBit32(args, nargs, 0);
    return Value::fromInt64(static_cast<int64_t>(x)).rawBits();
}

uint64_t bitBnot(void* /*cl*/, const uint64_t* args, int64_t nargs) {
    int32_t x = ~argBit32(args, nargs, 0);
    return Value::fromInt64(static_cast<int64_t>(x)).rawBits();
}

uint64_t bitBand(void* /*cl*/, const uint64_t* args, int64_t nargs) {
    uint32_t acc = 0xFFFFFFFFu;
    for (int64_t i = 0; i < nargs; ++i) {
        acc &= static_cast<uint32_t>(argBit32(args, nargs, i));
    }
    return Value::fromInt64(static_cast<int64_t>(static_cast<int32_t>(acc))).rawBits();
}

uint64_t bitBor(void* /*cl*/, const uint64_t* args, int64_t nargs) {
    uint32_t acc = 0u;
    for (int64_t i = 0; i < nargs; ++i) {
        acc |= static_cast<uint32_t>(argBit32(args, nargs, i));
    }
    return Value::fromInt64(static_cast<int64_t>(static_cast<int32_t>(acc))).rawBits();
}

uint64_t bitBxor(void* /*cl*/, const uint64_t* args, int64_t nargs) {
    uint32_t acc = 0u;
    for (int64_t i = 0; i < nargs; ++i) {
        acc ^= static_cast<uint32_t>(argBit32(args, nargs, i));
    }
    return Value::fromInt64(static_cast<int64_t>(static_cast<int32_t>(acc))).rawBits();
}

uint64_t bitLshift(void* /*cl*/, const uint64_t* args, int64_t nargs) {
    uint32_t x = static_cast<uint32_t>(argBit32(args, nargs, 0));
    uint32_t n = static_cast<uint32_t>(argBit32(args, nargs, 1)) & 31u;
    return Value::fromInt64(static_cast<int64_t>(static_cast<int32_t>(x << n))).rawBits();
}

uint64_t bitRshift(void* /*cl*/, const uint64_t* args, int64_t nargs) {
    uint32_t x = static_cast<uint32_t>(argBit32(args, nargs, 0));
    uint32_t n = static_cast<uint32_t>(argBit32(args, nargs, 1)) & 31u;
    return Value::fromInt64(static_cast<int64_t>(static_cast<int32_t>(x >> n))).rawBits();
}

uint64_t bitArshift(void* /*cl*/, const uint64_t* args, int64_t nargs) {
    int32_t x = argBit32(args, nargs, 0);
    uint32_t n = static_cast<uint32_t>(argBit32(args, nargs, 1)) & 31u;
    return Value::fromInt64(static_cast<int64_t>(x >> n)).rawBits();
}

uint64_t bitRol(void* /*cl*/, const uint64_t* args, int64_t nargs) {
    uint32_t x = static_cast<uint32_t>(argBit32(args, nargs, 0));
    uint32_t n = static_cast<uint32_t>(argBit32(args, nargs, 1)) & 31u;
    uint32_t r = (n == 0) ? x : ((x << n) | (x >> (32u - n)));
    return Value::fromInt64(static_cast<int64_t>(static_cast<int32_t>(r))).rawBits();
}

uint64_t bitRor(void* /*cl*/, const uint64_t* args, int64_t nargs) {
    uint32_t x = static_cast<uint32_t>(argBit32(args, nargs, 0));
    uint32_t n = static_cast<uint32_t>(argBit32(args, nargs, 1)) & 31u;
    uint32_t r = (n == 0) ? x : ((x >> n) | (x << (32u - n)));
    return Value::fromInt64(static_cast<int64_t>(static_cast<int32_t>(r))).rawBits();
}

uint64_t bitBswap(void* /*cl*/, const uint64_t* args, int64_t nargs) {
    uint32_t x = static_cast<uint32_t>(argBit32(args, nargs, 0));
    uint32_t r = ((x & 0x000000FFu) << 24) |
                 ((x & 0x0000FF00u) << 8)  |
                 ((x & 0x00FF0000u) >> 8)  |
                 ((x & 0xFF000000u) >> 24);
    return Value::fromInt64(static_cast<int64_t>(static_cast<int32_t>(r))).rawBits();
}

uint64_t bitTohex(void* /*cl*/, const uint64_t* args, int64_t nargs) {
    auto& rt = RuntimeState::current();
    uint32_t x = static_cast<uint32_t>(argBit32(args, nargs, 0));
    int32_t n = nargs >= 2 ? argBit32(args, nargs, 1) : 8;
    bool upper = n < 0;
    int32_t digits = upper ? -n : n;
    if (digits < 1) digits = 1;
    if (digits > 8) digits = 8;
    const char* hex = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    char buf[9];
    for (int32_t i = digits - 1; i >= 0; --i) {
        buf[i] = hex[x & 0xFu];
        x >>= 4;
    }
    return rt.makeStringValue(std::string_view(buf, static_cast<size_t>(digits)));
}

// ---- `os` Library ----

uint64_t osClock(void* /*cl*/, const uint64_t* /*args*/, int64_t /*nargs*/) {
    static const auto start = std::chrono::steady_clock::now();
    auto now = std::chrono::steady_clock::now();
    double secs = std::chrono::duration<double>(now - start).count();
    return Value::fromDouble(secs).rawBits();
}

uint64_t osTime(void* /*cl*/, const uint64_t* /*args*/, int64_t /*nargs*/) {
    return Value::fromInt64(static_cast<int64_t>(std::time(nullptr))).rawBits();
}

uint64_t osDifftime(void* /*cl*/, const uint64_t* args, int64_t nargs) {
    double t2 = argDouble(args, nargs, 0);
    double t1 = nargs >= 2 ? argDouble(args, nargs, 1) : 0.0;
    return Value::fromDouble(t2 - t1).rawBits();
}

uint64_t osGetenv(void* /*cl*/, const uint64_t* args, int64_t nargs) {
    if (nargs < 1 || !Value(args[0]).isString()) return kNilBits;
    auto& rt = RuntimeState::current();
    std::string key(valueToStringView(rt, Value(args[0])));
    const char* val = std::getenv(key.c_str());
    if (!val) return kNilBits;
    return rt.makeStringValue(val);
}

uint64_t osExit(void* /*cl*/, const uint64_t* args, int64_t nargs) {
    int code = 0;
    if (nargs >= 1) {
        Value v(args[0]);
        if (v.isBool()) code = v.asBool() ? 0 : 1;
        else if (v.isNumeric()) code = static_cast<int>(v.asInt64());
    }
    std::exit(code);
    return kNilBits;
}

} // namespace

void registerMathAndBitStdlib(RuntimeState& rt) {
    LuaTable* math_tbl = rt.allocTable(0, 32);
    setTableFieldStr(rt, math_tbl, "abs", makeNativeClosure(rt, &mathAbs, 1));
    setTableFieldStr(rt, math_tbl, "floor", makeNativeClosure(rt, &mathFloor, 1));
    setTableFieldStr(rt, math_tbl, "ceil", makeNativeClosure(rt, &mathCeil, 1));
    setTableFieldStr(rt, math_tbl, "sqrt", makeNativeClosure(rt, &mathSqrt, 1));
    setTableFieldStr(rt, math_tbl, "sin", makeNativeClosure(rt, &mathSin, 1));
    setTableFieldStr(rt, math_tbl, "cos", makeNativeClosure(rt, &mathCos, 1));
    setTableFieldStr(rt, math_tbl, "tan", makeNativeClosure(rt, &mathTan, 1));
    setTableFieldStr(rt, math_tbl, "asin", makeNativeClosure(rt, &mathAsin, 1));
    setTableFieldStr(rt, math_tbl, "acos", makeNativeClosure(rt, &mathAcos, 1));
    setTableFieldStr(rt, math_tbl, "atan", makeNativeClosure(rt, &mathAtan, 1));
    setTableFieldStr(rt, math_tbl, "atan2", makeNativeClosure(rt, &mathAtan, 2));
    setTableFieldStr(rt, math_tbl, "exp", makeNativeClosure(rt, &mathExp, 1));
    setTableFieldStr(rt, math_tbl, "log", makeNativeClosure(rt, &mathLog, 1));
    setTableFieldStr(rt, math_tbl, "log10", makeNativeClosure(rt, &mathLog10, 1));
    setTableFieldStr(rt, math_tbl, "fmod", makeNativeClosure(rt, &mathFmod, 2));
    setTableFieldStr(rt, math_tbl, "modf", makeNativeClosure(rt, &mathModf, 1));
    setTableFieldStr(rt, math_tbl, "deg", makeNativeClosure(rt, &mathDeg, 1));
    setTableFieldStr(rt, math_tbl, "rad", makeNativeClosure(rt, &mathRad, 1));
    setTableFieldStr(rt, math_tbl, "min", makeNativeClosure(rt, &mathMin, 1));
    setTableFieldStr(rt, math_tbl, "max", makeNativeClosure(rt, &mathMax, 1));
    setTableFieldStr(rt, math_tbl, "random", makeNativeClosure(rt, &mathRandom, 0));
    setTableFieldStr(rt, math_tbl, "randomseed", makeNativeClosure(rt, &mathRandomseed, 1));
    setTableFieldStr(rt, math_tbl, "type", makeNativeClosure(rt, &mathType, 1));
    setTableFieldStr(rt, math_tbl, "tointeger", makeNativeClosure(rt, &mathTointeger, 1));
    setTableFieldStr(rt, math_tbl, "ult", makeNativeClosure(rt, &mathUlt, 2));
    setTableFieldStr(rt, math_tbl, "pi", Value::fromDouble(3.14159265358979323846));
    setTableFieldStr(rt, math_tbl, "huge", Value::fromDouble(std::numeric_limits<double>::infinity()));
    setTableFieldStr(rt, math_tbl, "maxinteger", Value::fromInt64(std::numeric_limits<int64_t>::max()));
    setTableFieldStr(rt, math_tbl, "mininteger", Value::fromInt64(std::numeric_limits<int64_t>::min()));
    rt.setGlobal("math", Value::fromTable(math_tbl));

    LuaTable* bit_tbl = rt.allocTable(0, 16);
    setTableFieldStr(rt, bit_tbl, "tobit", makeNativeClosure(rt, &bitTobit, 1));
    setTableFieldStr(rt, bit_tbl, "bnot", makeNativeClosure(rt, &bitBnot, 1));
    setTableFieldStr(rt, bit_tbl, "band", makeNativeClosure(rt, &bitBand, 2));
    setTableFieldStr(rt, bit_tbl, "bor", makeNativeClosure(rt, &bitBor, 2));
    setTableFieldStr(rt, bit_tbl, "bxor", makeNativeClosure(rt, &bitBxor, 2));
    setTableFieldStr(rt, bit_tbl, "lshift", makeNativeClosure(rt, &bitLshift, 2));
    setTableFieldStr(rt, bit_tbl, "rshift", makeNativeClosure(rt, &bitRshift, 2));
    setTableFieldStr(rt, bit_tbl, "arshift", makeNativeClosure(rt, &bitArshift, 2));
    setTableFieldStr(rt, bit_tbl, "rol", makeNativeClosure(rt, &bitRol, 2));
    setTableFieldStr(rt, bit_tbl, "ror", makeNativeClosure(rt, &bitRor, 2));
    setTableFieldStr(rt, bit_tbl, "bswap", makeNativeClosure(rt, &bitBswap, 1));
    setTableFieldStr(rt, bit_tbl, "tohex", makeNativeClosure(rt, &bitTohex, 1));
    rt.setGlobal("bit", Value::fromTable(bit_tbl));
    rt.setGlobal("bit32", Value::fromTable(bit_tbl));

    LuaTable* os_tbl = rt.allocTable(0, 8);
    setTableFieldStr(rt, os_tbl, "clock", makeNativeClosure(rt, &osClock, 0));
    setTableFieldStr(rt, os_tbl, "time", makeNativeClosure(rt, &osTime, 0));
    setTableFieldStr(rt, os_tbl, "difftime", makeNativeClosure(rt, &osDifftime, 2));
    setTableFieldStr(rt, os_tbl, "getenv", makeNativeClosure(rt, &osGetenv, 1));
    setTableFieldStr(rt, os_tbl, "exit", makeNativeClosure(rt, &osExit, 1));
    rt.setGlobal("os", Value::fromTable(os_tbl));
}

} // namespace brasslua
