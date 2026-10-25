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
    kMetaNonTable   = 1u << 31,
};

struct LuaRuntimeError : public std::runtime_error {
    Value error_value;
    explicit LuaRuntimeError(std::string msg, Value v = Value::nil())
        : std::runtime_error(std::move(msg)), error_value(v) {}
};

// Internal runtime helpers shared across runtime_*.cpp and stdlib_*.cpp
bool isClosurePtr(RuntimeState& rt, const void* ptr) noexcept;
bool isUpvaluePtr(RuntimeState& rt, const void* ptr) noexcept;
uint32_t getSlotInternId(const RuntimeState& rt, uint32_t slot) noexcept;
void setStringMetatable(RuntimeState& rt, LuaTable* mt) noexcept;
LuaTable* getStringMetatable(RuntimeState& rt) noexcept;

LuaCoroutine* getRunningCoroutine(RuntimeState& rt) noexcept;
void setRunningCoroutine(RuntimeState& rt, LuaCoroutine* co) noexcept;

void recomputeMetatableFlags(RuntimeState& rt, LuaTable* tbl);
Value lookupMetamethod(RuntimeState& rt, Value obj, std::string_view event);
void armTableFinalizerIfNeeded(RuntimeState& rt, LuaTable* tbl);

bool tryCoerceToDouble(Value v, double& out) noexcept;
bool tryCoerceToInt64(Value v, int64_t& out) noexcept;
bool tryConvertFloatToExactInt64(double d, int64_t& out) noexcept;
std::string valueToStdString(RuntimeState& rt, Value v);
std::string_view valueToStringView(RuntimeState& rt, Value v);

Value makeNativeClosure(RuntimeState& rt, LuaNativeFn fn, uint32_t arity = 0);
void setTableFieldStr(RuntimeState& rt, LuaTable* tbl, std::string_view key, Value val);

// Multi-return helper: sets multiret buffer `[r0, r1, ...]` and returns `r0`
uint64_t returnMulti(RuntimeState& rt, const std::vector<uint64_t>& vals);

// Stdlib registration entry points
void registerCoreStdlib(RuntimeState& rt);
void registerMathAndBitStdlib(RuntimeState& rt);
void registerStringAndTableStdlib(RuntimeState& rt);

} // namespace brasslua
