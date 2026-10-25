#include "runtime/runtime_internal.h"

#include <brass/gc/heap.hpp>

#include <cmath>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

namespace brasslua {
namespace {

uint64_t callBinaryMetamethod(RuntimeState& rt, uint64_t a, uint64_t b, std::string_view event, const char* op_desc) {
    Value mm = lookupMetamethod(rt, Value(a), event);
    if (mm.isNil()) {
        mm = lookupMetamethod(rt, Value(b), event);
    }
    if (!mm.isNil()) {
        return brasslua_rt_call_2(mm.rawBits(), a, b);
    }
    throw LuaRuntimeError(std::string("attempt to ") + op_desc + " non-numeric values");
}

// Exact mathematical comparison between int64_t i and double d (Lua 5.4 lvm.c)
int compareIntFloat(int64_t i, double d) noexcept {
    if (std::isnan(d)) return 2; // unordered
    constexpr double kMaxIntBound = 9223372036854775808.0; // 2^63
    constexpr double kMinIntBound = -9223372036854775808.0; // -2^63
    if (d >= kMaxIntBound) return -1; // i < d
    if (d < kMinIntBound) return 1;   // i > d
    double di = static_cast<double>(i);
    if (di < d) return -1;
    if (di > d) return 1;
    // di == d: recover rounded-off integer difference for large integers near 2^53..2^63
    int64_t id = static_cast<int64_t>(d);
    if (i < id) return -1;
    if (i > id) return 1;
    return 0;
}

} // namespace
} // namespace brasslua

extern "C" {

using namespace brasslua;

uint64_t brasslua_rt_get_global(uint32_t intern_id) {
    return RuntimeState::current().getGlobalById(intern_id).rawBits();
}

void brasslua_rt_set_global(uint32_t intern_id, uint64_t val) {
    auto& rt = RuntimeState::current();
    rt.setGlobalById(intern_id, Value(val));
    if (rt.hostBridge().setGlobal) {
        rt.hostBridge().setGlobal(rt.getStringById(intern_id), val);
    }
}

uint64_t brasslua_rt_concat(uint64_t a, uint64_t b) {
    auto& rt = RuntimeState::current();
    Value va(a), vb(b);
    if ((va.isString() || va.isNumeric()) && (vb.isString() || vb.isNumeric())) {
        std::string sa = valueToStdString(rt, va);
        std::string sb = valueToStdString(rt, vb);
        sa.append(sb);
        return rt.makeStringValue(sa);
    }
    return callBinaryMetamethod(rt, a, b, "__concat", "concatenate");
}

int64_t brasslua_rt_obj_len(uint64_t v) {
    Value val(v);
    if (val.isString()) {
        auto* s = static_cast<const LuaString*>(val.asPointer());
        return s ? static_cast<int64_t>(s->length) : 0;
    }
    if (val.isTable()) {
        return brasslua_rt_table_len(v);
    }
    auto& rt = RuntimeState::current();
    if (rt.hostBridge().isHostObject && rt.hostBridge().isHostObject(v) && rt.hostBridge().getProperty) {
        uint64_t len_val = rt.hostBridge().getProperty(v, "length");
        if (Value(len_val).isNumeric()) {
            return Value(len_val).asInt64();
        }
    }
    Value mm = lookupMetamethod(rt, val, "__len");
    if (!mm.isNil()) {
        uint64_t res = brasslua_rt_call_1(mm.rawBits(), v);
        return Value(res).asInt64();
    }
    throw LuaRuntimeError("attempt to get length of a non-string/table value");
}

uint64_t brasslua_rt_closure_new(void* fn_ptr, void* unboxed_ptr, uint32_t upval_count, uint32_t arity) {
    auto& rt = RuntimeState::current();
    LuaClosure* cl = rt.allocClosure(reinterpret_cast<LuaNativeFn>(fn_ptr), unboxed_ptr, upval_count, arity, false);
    return Value::fromFunction(cl).rawBits();
}

void* brasslua_rt_upvalue_new(uint64_t initial_val) {
    return RuntimeState::current().allocUpvalue(initial_val);
}

uint64_t brasslua_rt_upvalue_get(void* closure_or_upval, uint32_t idx) {
    if (!closure_or_upval) return kNilBits;
    auto& rt = RuntimeState::current();
    if (isUpvaluePtr(rt, closure_or_upval)) {
        return static_cast<LuaUpvalue*>(closure_or_upval)->value;
    }
    if (isClosurePtr(rt, closure_or_upval)) {
        auto* cl = static_cast<LuaClosure*>(closure_or_upval);
        if (idx < cl->upvalue_count && cl->upvalues[idx]) {
            return cl->upvalues[idx]->value;
        }
    }
    return kNilBits;
}

void brasslua_rt_upvalue_set(void* closure_or_upval, uint32_t idx, uint64_t val) {
    if (!closure_or_upval) return;
    auto& rt = RuntimeState::current();
    if (isUpvaluePtr(rt, closure_or_upval)) {
        auto* uv = static_cast<LuaUpvalue*>(closure_or_upval);
        uv->value = val;
        rt.heap().write_barrier(reinterpret_cast<uintptr_t>(uv), val);
        return;
    }
    if (isClosurePtr(rt, closure_or_upval)) {
        auto* cl = static_cast<LuaClosure*>(closure_or_upval);
        if (idx < cl->upvalue_count && cl->upvalues[idx]) {
            cl->upvalues[idx]->value = val;
            rt.heap().write_barrier(reinterpret_cast<uintptr_t>(cl->upvalues[idx]), val);
        }
    }
}

void brasslua_rt_close_local(uint64_t val) {
    Value v(val);
    if (!v.isTruthy()) return; // nil and false are ignored by <close> in Lua 5.4
    auto& rt = RuntimeState::current();
    Value mm = lookupMetamethod(rt, v, "__close");
    if (mm.isNil()) {
        throw LuaRuntimeError("variable <close> has no '__close' metamethod");
    }
    brasslua_rt_call_2(mm.rawBits(), val, kNilBits);
}

#if defined(_MSC_VER)
#define BRASSLUA_NOINLINE __declspec(noinline)
#else
#define BRASSLUA_NOINLINE __attribute__((noinline))
#endif

static BRASSLUA_NOINLINE uint64_t callNonFunctionSlow(
    RuntimeState& rt, uint64_t callee, const uint64_t* args, int64_t nargs) {
    if (rt.hostBridge().isHostObject && rt.hostBridge().isHostObject(callee) && rt.hostBridge().callHost) {
        rt.clearMultiRet();
        uint64_t r0 = rt.hostBridge().callHost(callee, kNilBits, args, static_cast<size_t>(nargs));
        if (!rt.isMultiRetSet()) {
            rt.setMultiRet(&r0, 1);
        }
        return r0;
    }
    Value mm = lookupMetamethod(rt, Value(callee), "__call");
    if (!mm.isNil()) {
        std::vector<uint64_t> full_args;
        full_args.reserve(static_cast<size_t>(nargs) + 1);
        full_args.push_back(callee);
        for (int64_t i = 0; i < nargs; ++i) {
            full_args.push_back(args[i]);
        }
        return brasslua_rt_call(mm.rawBits(), full_args.data(), nargs + 1);
    }
    throw LuaRuntimeError("attempt to call a non-function value");
}

uint64_t brasslua_rt_call(uint64_t callee, const uint64_t* args, int64_t nargs) {
    auto& rt = RuntimeState::current();
    Value cv(callee);
    if (cv.isFunction()) {
        auto* cl = static_cast<LuaClosure*>(cv.asPointer());
        if (cl && cl->fn) {
            rt.clearMultiRet();
            uint64_t r0 = cl->fn(cl, args, nargs);
            if (!rt.isMultiRetSet()) {
                rt.setSingleRet(r0);
            }
            return r0;
        }
    }
    return callNonFunctionSlow(rt, callee, args, nargs);
}

uint64_t brasslua_rt_call_0(uint64_t callee) {
    auto& rt = RuntimeState::current();
    Value cv(callee);
    if (cv.isFunction()) {
        auto* cl = static_cast<LuaClosure*>(cv.asPointer());
        if (cl && cl->fn) {
            rt.clearMultiRet();
            uint64_t r0 = cl->fn(cl, nullptr, 0);
            if (!rt.isMultiRetSet()) {
                rt.setSingleRet(r0);
            }
            return r0;
        }
    }
    return callNonFunctionSlow(rt, callee, nullptr, 0);
}

uint64_t brasslua_rt_call_1(uint64_t callee, uint64_t a0) {
    auto& rt = RuntimeState::current();
    Value cv(callee);
    if (cv.isFunction()) {
        auto* cl = static_cast<LuaClosure*>(cv.asPointer());
        if (cl && cl->fn) {
            rt.clearMultiRet();
            uint64_t r0 = cl->fn(cl, &a0, 1);
            if (!rt.isMultiRetSet()) {
                rt.setSingleRet(r0);
            }
            return r0;
        }
    }
    return callNonFunctionSlow(rt, callee, &a0, 1);
}

uint64_t brasslua_rt_call_2(uint64_t callee, uint64_t a0, uint64_t a1) {
    uint64_t args[2] = {a0, a1};
    auto& rt = RuntimeState::current();
    Value cv(callee);
    if (cv.isFunction()) {
        auto* cl = static_cast<LuaClosure*>(cv.asPointer());
        if (cl && cl->fn) {
            rt.clearMultiRet();
            uint64_t r0 = cl->fn(cl, args, 2);
            if (!rt.isMultiRetSet()) {
                rt.setSingleRet(r0);
            }
            return r0;
        }
    }
    return callNonFunctionSlow(rt, callee, args, 2);
}

uint64_t brasslua_rt_call_3(uint64_t callee, uint64_t a0, uint64_t a1, uint64_t a2) {
    uint64_t args[3] = {a0, a1, a2};
    auto& rt = RuntimeState::current();
    Value cv(callee);
    if (cv.isFunction()) {
        auto* cl = static_cast<LuaClosure*>(cv.asPointer());
        if (cl && cl->fn) {
            rt.clearMultiRet();
            uint64_t r0 = cl->fn(cl, args, 3);
            if (!rt.isMultiRetSet()) {
                rt.setSingleRet(r0);
            }
            return r0;
        }
    }
    return callNonFunctionSlow(rt, callee, args, 3);
}

uint64_t brasslua_rt_call_4(uint64_t callee, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3) {
    uint64_t args[4] = {a0, a1, a2, a3};
    auto& rt = RuntimeState::current();
    Value cv(callee);
    if (cv.isFunction()) {
        auto* cl = static_cast<LuaClosure*>(cv.asPointer());
        if (cl && cl->fn) {
            rt.clearMultiRet();
            uint64_t r0 = cl->fn(cl, args, 4);
            if (!rt.isMultiRetSet()) {
                rt.setSingleRet(r0);
            }
            return r0;
        }
    }
    return callNonFunctionSlow(rt, callee, args, 4);
}

uint64_t brasslua_rt_call_multiret(uint64_t callee, const uint64_t* prefix_args, int64_t prefix_count) {
    auto& rt = RuntimeState::current();
    const auto& tail = rt.multiRetVec();
    size_t pcount = prefix_count > 0 ? static_cast<size_t>(prefix_count) : 0;
    size_t total = pcount + tail.size();
    uint64_t stack_buf[16];
    std::vector<uint64_t> heap_buf;
    uint64_t* full_args = stack_buf;
    if (total > 16) {
        heap_buf.resize(total);
        full_args = heap_buf.data();
    }
    for (size_t i = 0; i < pcount; ++i) {
        full_args[i] = prefix_args[i];
    }
    for (size_t i = 0; i < tail.size(); ++i) {
        full_args[pcount + i] = tail[i];
    }
    return brasslua_rt_call(callee, full_args, static_cast<int64_t>(total));
}

uint64_t brasslua_rt_call_method(uint64_t obj, uint64_t method_key, const uint64_t* args, int64_t nargs) {
    auto& rt = RuntimeState::current();
    // Rule 6: When calling a method on a host object (`obj:m(x)`), pass `obj` as the
    // explicit receiver (`this`) and `(x)` as the arguments! (args[0] is obj.)
    if (rt.hostBridge().isHostObject && rt.hostBridge().isHostObject(obj)) {
        std::string_view mname = valueToStringView(rt, Value(method_key));
        const size_t host_nargs = nargs > 0 ? static_cast<size_t>(nargs - 1) : 0;
        if (rt.hostBridge().callMethod) {
            rt.clearMultiRet();
            uint64_t r0 = rt.hostBridge().callMethod(obj, mname, args + 1, host_nargs);
            if (!rt.isMultiRetSet()) rt.setMultiRet(&r0, 1);
            return r0;
        }
        if (rt.hostBridge().getProperty && rt.hostBridge().callHost) {
            uint64_t fn = rt.hostBridge().getProperty(obj, mname);
            rt.clearMultiRet();
            uint64_t r0 = rt.hostBridge().callHost(fn, obj, args + 1, host_nargs);
            if (!rt.isMultiRetSet()) rt.setMultiRet(&r0, 1);
            return r0;
        }
    }
    // Pure Lua table/value: look up `obj[method]` and call it with `obj` as `self` (arg 1).
    uint64_t callee = brasslua_rt_table_get_field(obj, method_key);
    return brasslua_rt_call(callee, args, nargs);
}

uint64_t brasslua_rt_call_method_multiret(uint64_t obj, uint64_t method_key,
                                          const uint64_t* prefix_args, int64_t prefix_count) {
    auto& rt = RuntimeState::current();
    const size_t tail_count = rt.multiRetCount();
    size_t pcount = prefix_count > 0 ? static_cast<size_t>(prefix_count) : 0;
    size_t total = pcount + tail_count;
    uint64_t stack_buf[16];
    std::vector<uint64_t> heap_buf;
    uint64_t* full_args = stack_buf;
    if (total > 16) {
        heap_buf.resize(total);
        full_args = heap_buf.data();
    }
    for (size_t i = 0; i < pcount; ++i) {
        full_args[i] = prefix_args[i];
    }
    for (size_t i = 0; i < tail_count; ++i) {
        full_args[pcount + i] = rt.getMultiRet(i);
    }
    return brasslua_rt_call_method(obj, method_key, full_args, static_cast<int64_t>(total));
}

void brasslua_rt_set_multiret(const uint64_t* vals, int64_t count) {
    RuntimeState::current().setMultiRet(vals, count > 0 ? static_cast<size_t>(count) : 0);
}

void brasslua_rt_clear_multiret(void) {
    RuntimeState::current().clearMultiRet();
}

uint64_t brasslua_rt_prepend_multiret(const uint64_t* prefix_vals, int64_t prefix_count) {
    auto& rt = RuntimeState::current();
    const size_t tail_count = rt.multiRetCount();
    size_t pcount = prefix_count > 0 ? static_cast<size_t>(prefix_count) : 0;
    std::vector<uint64_t> combined;
    combined.reserve(pcount + tail_count);
    for (size_t i = 0; i < pcount; ++i) {
        combined.push_back(prefix_vals[i]);
    }
    for (size_t i = 0; i < tail_count; ++i) {
        combined.push_back(rt.getMultiRet(i));
    }
    rt.setMultiRet(combined.data(), combined.size());
    return combined.empty() ? kNilBits : combined[0];
}

uint64_t brasslua_rt_set_vararg_multiret(const uint64_t* args_ptr, int64_t nargs, int64_t vararg_offset) {
    auto& rt = RuntimeState::current();
    if (!args_ptr || nargs <= vararg_offset) {
        rt.setMultiRet(nullptr, 0);
        return kNilBits;
    }
    int64_t count = nargs - vararg_offset;
    const uint64_t* start = args_ptr + vararg_offset;
    rt.setMultiRet(start, static_cast<size_t>(count));
    return start[0];
}

int64_t brasslua_rt_get_multiret_count(void) {
    return static_cast<int64_t>(RuntimeState::current().multiRetCount());
}

uint64_t brasslua_rt_get_multiret(int64_t idx) {
    if (idx < 0) return kNilBits;
    return RuntimeState::current().getMultiRet(static_cast<size_t>(idx));
}

int64_t brasslua_rt_idiv_i64(int64_t a, int64_t b) {
    if (b == 0) {
        throw LuaRuntimeError("attempt to divide by zero");
    }
    if (a == std::numeric_limits<int64_t>::min() && b == -1) {
        return std::numeric_limits<int64_t>::min();
    }
    int64_t q = a / b;
    int64_t r = a % b;
    if (r != 0 && ((a ^ b) < 0)) {
        --q;
    }
    return q;
}

int64_t brasslua_rt_mod_i64(int64_t a, int64_t b) {
    if (b == 0) {
        throw LuaRuntimeError("attempt to perform 'n%0'");
    }
    if (a == std::numeric_limits<int64_t>::min() && b == -1) {
        return 0;
    }
    int64_t r = a % b;
    if (r != 0 && ((r ^ b) < 0)) {
        r += b;
    }
    return r;
}

uint64_t brasslua_rt_add(uint64_t a, uint64_t b) {
    Value va(a), vb(b);
    if (va.isInteger() && vb.isInteger()) {
        int64_t res = static_cast<int64_t>(static_cast<uint64_t>(va.asInt64()) +
                                           static_cast<uint64_t>(vb.asInt64()));
        return Value::fromInt64(res).rawBits();
    }
    double da = 0.0, db = 0.0;
    if (tryCoerceToDouble(va, da) && tryCoerceToDouble(vb, db)) {
        return Value::fromDouble(da + db).rawBits();
    }
    return callBinaryMetamethod(RuntimeState::current(), a, b, "__add", "add");
}

uint64_t brasslua_rt_sub(uint64_t a, uint64_t b) {
    Value va(a), vb(b);
    if (va.isInteger() && vb.isInteger()) {
        int64_t res = static_cast<int64_t>(static_cast<uint64_t>(va.asInt64()) -
                                           static_cast<uint64_t>(vb.asInt64()));
        return Value::fromInt64(res).rawBits();
    }
    double da = 0.0, db = 0.0;
    if (tryCoerceToDouble(va, da) && tryCoerceToDouble(vb, db)) {
        return Value::fromDouble(da - db).rawBits();
    }
    return callBinaryMetamethod(RuntimeState::current(), a, b, "__sub", "subtract");
}

uint64_t brasslua_rt_mul(uint64_t a, uint64_t b) {
    Value va(a), vb(b);
    if (va.isInteger() && vb.isInteger()) {
        int64_t res = static_cast<int64_t>(static_cast<uint64_t>(va.asInt64()) *
                                           static_cast<uint64_t>(vb.asInt64()));
        return Value::fromInt64(res).rawBits();
    }
    double da = 0.0, db = 0.0;
    if (tryCoerceToDouble(va, da) && tryCoerceToDouble(vb, db)) {
        return Value::fromDouble(da * db).rawBits();
    }
    return callBinaryMetamethod(RuntimeState::current(), a, b, "__mul", "multiply");
}

uint64_t brasslua_rt_div(uint64_t a, uint64_t b) {
    double da = 0.0, db = 0.0;
    if (tryCoerceToDouble(Value(a), da) && tryCoerceToDouble(Value(b), db)) {
        return Value::fromDouble(da / db).rawBits();
    }
    return callBinaryMetamethod(RuntimeState::current(), a, b, "__div", "divide");
}

uint64_t brasslua_rt_idiv(uint64_t a, uint64_t b) {
    Value va(a), vb(b);
    if (va.isInteger() && vb.isInteger()) {
        return Value::fromInt64(brasslua_rt_idiv_i64(va.asInt64(), vb.asInt64())).rawBits();
    }
    double da = 0.0, db = 0.0;
    if (tryCoerceToDouble(va, da) && tryCoerceToDouble(vb, db)) {
        return Value::fromDouble(std::floor(da / db)).rawBits();
    }
    return callBinaryMetamethod(RuntimeState::current(), a, b, "__idiv", "floor-divide");
}

double brasslua_rt_fmod(double a, double b) {
    double r = std::fmod(a, b);
    if (r != 0.0 && ((r < 0.0) != (b < 0.0))) {
        r += b;
    }
    return r;
}

uint64_t brasslua_rt_mod(uint64_t a, uint64_t b) {
    Value va(a), vb(b);
    if (va.isInteger() && vb.isInteger()) {
        return Value::fromInt64(brasslua_rt_mod_i64(va.asInt64(), vb.asInt64())).rawBits();
    }
    double da = 0.0, db = 0.0;
    if (tryCoerceToDouble(va, da) && tryCoerceToDouble(vb, db)) {
        return Value::fromDouble(brasslua_rt_fmod(da, db)).rawBits();
    }
    return callBinaryMetamethod(RuntimeState::current(), a, b, "__mod", "modulo");
}

double brasslua_rt_pow_f64(double a, double b) {
    return std::pow(a, b);
}

uint64_t brasslua_rt_pow(uint64_t a, uint64_t b) {
    double da = 0.0, db = 0.0;
    if (tryCoerceToDouble(Value(a), da) && tryCoerceToDouble(Value(b), db)) {
        return Value::fromDouble(std::pow(da, db)).rawBits();
    }
    return callBinaryMetamethod(RuntimeState::current(), a, b, "__pow", "exponentiate");
}

uint64_t brasslua_rt_unm(uint64_t a) {
    Value va(a);
    if (va.isInteger()) {
        int64_t neg = static_cast<int64_t>(0ULL - static_cast<uint64_t>(va.asInt64()));
        return Value::fromInt64(neg).rawBits();
    }
    double da = 0.0;
    if (tryCoerceToDouble(va, da)) {
        return Value::fromDouble(-da).rawBits();
    }
    auto& rt = RuntimeState::current();
    Value mm = lookupMetamethod(rt, va, "__unm");
    if (!mm.isNil()) {
        return brasslua_rt_call_1(mm.rawBits(), a);
    }
    throw LuaRuntimeError("attempt to negate a non-numeric value");
}

uint64_t brasslua_rt_band(uint64_t a, uint64_t b) {
    int64_t ia = 0, ib = 0;
    if (tryCoerceToInt64(Value(a), ia) && tryCoerceToInt64(Value(b), ib)) {
        return Value::fromInt64(ia & ib).rawBits();
    }
    return callBinaryMetamethod(RuntimeState::current(), a, b, "__band", "bitwise AND");
}

uint64_t brasslua_rt_bor(uint64_t a, uint64_t b) {
    int64_t ia = 0, ib = 0;
    if (tryCoerceToInt64(Value(a), ia) && tryCoerceToInt64(Value(b), ib)) {
        return Value::fromInt64(ia | ib).rawBits();
    }
    return callBinaryMetamethod(RuntimeState::current(), a, b, "__bor", "bitwise OR");
}

uint64_t brasslua_rt_bxor(uint64_t a, uint64_t b) {
    int64_t ia = 0, ib = 0;
    if (tryCoerceToInt64(Value(a), ia) && tryCoerceToInt64(Value(b), ib)) {
        return Value::fromInt64(ia ^ ib).rawBits();
    }
    return callBinaryMetamethod(RuntimeState::current(), a, b, "__bxor", "bitwise XOR");
}

uint64_t brasslua_rt_bnot(uint64_t a) {
    int64_t ia = 0;
    if (tryCoerceToInt64(Value(a), ia)) {
        return Value::fromInt64(~ia).rawBits();
    }
    auto& rt = RuntimeState::current();
    Value mm = lookupMetamethod(rt, Value(a), "__bnot");
    if (!mm.isNil()) {
        return brasslua_rt_call_1(mm.rawBits(), a);
    }
    throw LuaRuntimeError("number has no integer representation");
}

static int64_t luaShiftLeft(int64_t x, int64_t y) noexcept {
    uint64_t ux = static_cast<uint64_t>(x);
    if (y < 0) {
        if (y <= -64) return 0;
        return static_cast<int64_t>(ux >> static_cast<uint64_t>(-y));
    }
    if (y >= 64) return 0;
    return static_cast<int64_t>(ux << static_cast<uint64_t>(y));
}

uint64_t brasslua_rt_shl(uint64_t a, uint64_t b) {
    int64_t ia = 0, ib = 0;
    if (tryCoerceToInt64(Value(a), ia) && tryCoerceToInt64(Value(b), ib)) {
        return Value::fromInt64(luaShiftLeft(ia, ib)).rawBits();
    }
    return callBinaryMetamethod(RuntimeState::current(), a, b, "__shl", "shift left");
}

uint64_t brasslua_rt_shr(uint64_t a, uint64_t b) {
    int64_t ia = 0, ib = 0;
    if (tryCoerceToInt64(Value(a), ia) && tryCoerceToInt64(Value(b), ib)) {
        int64_t neg_b = (ib == std::numeric_limits<int64_t>::min()) ? 64 : -ib;
        return Value::fromInt64(luaShiftLeft(ia, neg_b)).rawBits();
    }
    return callBinaryMetamethod(RuntimeState::current(), a, b, "__shr", "shift right");
}

int32_t brasslua_rt_eq(uint64_t a, uint64_t b) {
    Value va(a), vb(b);
    if (va.isInteger() && vb.isInteger()) {
        return va.asInt64() == vb.asInt64() ? 1 : 0;
    }
    if (va.isFloat() && vb.isFloat()) {
        return va.asDouble() == vb.asDouble() ? 1 : 0;
    }
    if (va.isInteger() && vb.isFloat()) {
        return compareIntFloat(va.asInt64(), vb.asDouble()) == 0 ? 1 : 0;
    }
    if (va.isFloat() && vb.isInteger()) {
        return compareIntFloat(vb.asInt64(), va.asDouble()) == 0 ? 1 : 0;
    }
    if (va == vb) return 1;
    if (va.isString() && vb.isString()) {
        auto* sa = static_cast<const LuaString*>(va.asPointer());
        auto* sb = static_cast<const LuaString*>(vb.asPointer());
        if (sa == sb) return 1;
        if (!sa || !sb || sa->length != sb->length || sa->hash != sb->hash) return 0;
        return std::memcmp(sa->data, sb->data, sa->length) == 0 ? 1 : 0;
    }
    if ((va.isTable() && vb.isTable()) || (va.isFullUserdata() && vb.isFullUserdata())) {
        auto& rt = RuntimeState::current();
        Value mm = lookupMetamethod(rt, va, "__eq");
        if (mm.isNil()) {
            mm = lookupMetamethod(rt, vb, "__eq");
        }
        if (!mm.isNil()) {
            uint64_t res = brasslua_rt_call_2(mm.rawBits(), a, b);
            return Value(res).isTruthy() ? 1 : 0;
        }
    }
    return 0;
}

int32_t brasslua_rt_lt(uint64_t a, uint64_t b) {
    Value va(a), vb(b);
    if (va.isInteger() && vb.isInteger()) {
        return va.asInt64() < vb.asInt64() ? 1 : 0;
    }
    if (va.isFloat() && vb.isFloat()) {
        return va.asDouble() < vb.asDouble() ? 1 : 0;
    }
    if (va.isInteger() && vb.isFloat()) {
        int c = compareIntFloat(va.asInt64(), vb.asDouble());
        return c == -1 ? 1 : 0;
    }
    if (va.isFloat() && vb.isInteger()) {
        int c = compareIntFloat(vb.asInt64(), va.asDouble());
        return c == 1 ? 1 : 0;
    }
    if (va.isString() && vb.isString()) {
        auto* sa = static_cast<const LuaString*>(va.asPointer());
        auto* sb = static_cast<const LuaString*>(vb.asPointer());
        std::string_view sva(sa ? sa->data : "", sa ? sa->length : 0);
        std::string_view svb(sb ? sb->data : "", sb ? sb->length : 0);
        return sva < svb ? 1 : 0;
    }
    auto& rt = RuntimeState::current();
    Value mm = lookupMetamethod(rt, va, "__lt");
    if (mm.isNil()) mm = lookupMetamethod(rt, vb, "__lt");
    if (!mm.isNil()) {
        uint64_t res = brasslua_rt_call_2(mm.rawBits(), a, b);
        return Value(res).isTruthy() ? 1 : 0;
    }
    throw LuaRuntimeError("attempt to compare incompatible values");
}

int32_t brasslua_rt_le(uint64_t a, uint64_t b) {
    Value va(a), vb(b);
    if (va.isInteger() && vb.isInteger()) {
        return va.asInt64() <= vb.asInt64() ? 1 : 0;
    }
    if (va.isFloat() && vb.isFloat()) {
        return va.asDouble() <= vb.asDouble() ? 1 : 0;
    }
    if (va.isInteger() && vb.isFloat()) {
        int c = compareIntFloat(va.asInt64(), vb.asDouble());
        return (c == -1 || c == 0) ? 1 : 0;
    }
    if (va.isFloat() && vb.isInteger()) {
        int c = compareIntFloat(vb.asInt64(), va.asDouble());
        return (c == 1 || c == 0) ? 1 : 0;
    }
    if (va.isString() && vb.isString()) {
        auto* sa = static_cast<const LuaString*>(va.asPointer());
        auto* sb = static_cast<const LuaString*>(vb.asPointer());
        std::string_view sva(sa ? sa->data : "", sa ? sa->length : 0);
        std::string_view svb(sb ? sb->data : "", sb ? sb->length : 0);
        return sva <= svb ? 1 : 0;
    }
    auto& rt = RuntimeState::current();
    Value mm = lookupMetamethod(rt, va, "__le");
    if (mm.isNil()) mm = lookupMetamethod(rt, vb, "__le");
    if (!mm.isNil()) {
        uint64_t res = brasslua_rt_call_2(mm.rawBits(), a, b);
        return Value(res).isTruthy() ? 1 : 0;
    }
    mm = lookupMetamethod(rt, vb, "__lt");
    if (mm.isNil()) mm = lookupMetamethod(rt, va, "__lt");
    if (!mm.isNil()) {
        uint64_t res = brasslua_rt_call_2(mm.rawBits(), b, a);
        return Value(res).isTruthy() ? 0 : 1;
    }
    throw LuaRuntimeError("attempt to compare incompatible values");
}

double brasslua_rt_to_number(uint64_t v) {
    double d = 0.0;
    if (tryCoerceToDouble(Value(v), d)) return d;
    return 0.0;
}

int64_t brasslua_rt_to_int64(uint64_t v) {
    int64_t i = 0;
    if (tryCoerceToInt64(Value(v), i)) return i;
    return 0;
}

} // extern "C"
