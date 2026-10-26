#pragma once

#include "brasslua/runtime.h"
#include <stdexcept>
#include <string>
#include <string_view>

namespace brasslua {

// Metamethod & table flag bits on LuaTable::flags
enum MetaFlag : uint32_t {
    kMetaIndex      = 1u << 0,
    kMetaNewIndex   = 1u << 1,
    kMetaLen        = 1u << 2,
    kMetaEq         = 1u << 3,
    kMetaLt         = 1u << 4,
    kMetaLe         = 1u << 5,
    kMetaAdd        = 1u << 6,
    kMetaSub        = 1u << 7,
    kMetaMul        = 1u << 8,
    kMetaDiv        = 1u << 9,
    kMetaIDiv       = 1u << 10,
    kMetaMod        = 1u << 11,
    kMetaPow        = 1u << 12,
    kMetaUnm        = 1u << 13,
    kMetaConcat     = 1u << 14,
    kMetaCall       = 1u << 15,
    kMetaToString   = 1u << 16,
    kMetaBand       = 1u << 17,
    kMetaBor        = 1u << 18,
    kMetaBxor       = 1u << 19,
    kMetaBnot       = 1u << 20,
    kMetaShl        = 1u << 21,
    kMetaShr        = 1u << 22,
    kMetaClose      = 1u << 23,
    kMetaGc         = 1u << 24,
    kMetaWeakKeys   = 1u << 25,
    kMetaWeakValues = 1u << 26,
    kMetaGcArmed    = 1u << 27,
    kMetaProtected  = 1u << 28,
    kMetaCacheValid = 1u << 29,
    kMetaNonTable   = 1u << 31,
    kMetaTableFlagsMask = (1u << 27) - 1u,
};

inline uint32_t hashLuaKey(uint64_t key_bits) noexcept {
    Value k(key_bits);
    if (k.isString()) {
        return static_cast<const LuaString*>(k.asPointer())->hash;
    }
    uint64_t x = k.isBoxedInt64() ? static_cast<uint64_t>(k.asInt64()) : key_bits;
    x ^= x >> 30;
    x *= 0xbf58476d1ce4e5b9ULL;
    x ^= x >> 27;
    return static_cast<uint32_t>(x ^ (x >> 31));
}

struct LuaRuntimeError : public std::runtime_error {
    Value error_value;
    explicit LuaRuntimeError(std::string msg, Value v = Value::nil())
        : std::runtime_error(std::move(msg)), error_value(v) {}
};

// Internal runtime helpers shared across runtime_*.cpp and stdlib_*.cpp
bool isClosurePtr(RuntimeState& rt, const void* ptr) noexcept;
bool isUpvaluePtr(RuntimeState& rt, const void* ptr) noexcept;
void setStringMetatable(RuntimeState& rt, LuaTable* mt) noexcept;
LuaTable* getStringMetatable(RuntimeState& rt) noexcept;

LuaCoroutine* getRunningCoroutine(RuntimeState& rt) noexcept;
void setRunningCoroutine(RuntimeState& rt, LuaCoroutine* co) noexcept;
// The main thread as a thread value (coroutine.running outside any
// coroutine). Its status stays Running: coroutine.status says "normal" for
// it while a coroutine runs.
LuaCoroutine* getMainThread(RuntimeState& rt);

void recomputeMetatableFlags(RuntimeState& rt, LuaTable* tbl);
uint32_t getOrComputeMetatableFlags(RuntimeState& rt, LuaTable* mt);
Value lookupMetamethod(RuntimeState& rt, Value obj, std::string_view event);
void armTableFinalizerIfNeeded(RuntimeState& rt, LuaTable* tbl);
// The capacity allocTable gives the hash part of a table made for `entries`
// keys (0 for none): a power of two at least twice the keys.
uint32_t tableHashCapFor(uint32_t entries) noexcept;
// The raw length `#t` (a border, no __len): O(1) when array_len still names
// one, else a binary search of the array part. Keeps array_len as the hint.
int64_t rawTableLength(LuaTable* tbl) noexcept;

// A string's number as Lua reads one (luaO_str2num): a decimal integer that
// fits or a hex one (wrapping) is an integer, else strtod's reading of the
// whole string (no 'inf' or 'nan'), spaces allowed about it. lua_number.cpp.
bool stringToNumber(std::string_view s, Value& out) noexcept;
// tonumber(s, base)'s reading: an optional '-', then base-`base` digits.
bool stringToIntegerInBase(std::string_view s, int base, int64_t& out) noexcept;
bool tryCoerceToDouble(Value v, double& out) noexcept;
bool tryCoerceToInt64(Value v, int64_t& out) noexcept;
// luaG_typeerror: "attempt to <op> a <type> value", a table's or
// userdata's __name its type.
[[noreturn]] void throwTypeError(RuntimeState& rt, Value v, const char* op);
bool tryConvertFloatToExactInt64(double d, int64_t& out) noexcept;
// A number's string form as Lua writes it (tostring, .., a number passed as
// a string): "%d" for an integer, "%.14g" for a float with ".0" after one
// that reads as an integer. `buf` holds kNumberBufSize bytes; returns the
// length written (NUL-terminated).
inline constexpr size_t kNumberBufSize = 64;
size_t numberToChars(Value v, char* buf) noexcept;
std::string valueToStdString(RuntimeState& rt, Value v);
std::string_view valueToStringView(RuntimeState& rt, Value v);

Value makeNativeClosure(RuntimeState& rt, LuaNativeFn fn, uint32_t arity = 0);
void setTableFieldStr(RuntimeState& rt, LuaTable* tbl, std::string_view key, Value val);

// Multi-return helper: sets multiret buffer `[r0, r1, ...]` and returns `r0`
uint64_t returnMulti(RuntimeState& rt, const std::vector<uint64_t>& vals);

// Stdlib registration entry points
void registerCoreStdlib(RuntimeState& rt);
void registerMathAndBitStdlib(RuntimeState& rt);
void registerStringStdlib(RuntimeState& rt);
void registerTableStdlib(RuntimeState& rt);
void registerIoStdlib(RuntimeState& rt);

} // namespace brasslua
