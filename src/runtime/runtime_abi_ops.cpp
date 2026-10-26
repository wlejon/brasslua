#include "runtime/runtime_internal.h"
#include "runtime/stdlib_aux.h"

#include <brass/gc/heap.hpp>

#include <cmath>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

namespace brasslua {
namespace {

enum class BinOpKind { Arith, Bitwise, Concat };
[[noreturn]] void throwBinOpError(RuntimeState& rt, Value a, Value b, BinOpKind kind);

// luaT_trybinTM: the metamethod of a, else b's, else the operator's error.
uint64_t callBinaryMetamethod(RuntimeState& rt, uint64_t a, uint64_t b, std::string_view event, BinOpKind kind) {
    Value mm = lookupMetamethod(rt, Value(a), event);
    if (mm.isNil()) {
        mm = lookupMetamethod(rt, Value(b), event);
    }
    if (!mm.isNil()) {
        return brasslua_rt_call_2(mm.rawBits(), a, b);
    }
    throwBinOpError(rt, Value(a), Value(b), kind);
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

// An arithmetic operand: a number, or a string read as one (Lua's string
// coercion keeps "10" an integer).
bool arithOperand(Value v, Value& out) noexcept {
    if (v.isNumeric()) {
        out = v;
        return true;
    }
    if (v.isString()) {
        auto* s = static_cast<const LuaString*>(v.asPointer());
        return s && stringToNumber(std::string_view(s->data, s->length), out);
    }
    return false;
}

double asDouble(Value v) noexcept {
    return v.isInteger() ? static_cast<double>(v.asInt64()) : v.asDouble();
}

// a op b for the operators with an integer form: integers stay integers.
template <typename IntOp, typename FloatOp>
bool arith(uint64_t a, uint64_t b, IntOp int_op, FloatOp float_op, uint64_t& out) {
    Value na, nb;
    if (!arithOperand(Value(a), na) || !arithOperand(Value(b), nb)) return false;
    if (na.isInteger() && nb.isInteger()) {
        out = Value::fromInt64(int_op(na.asInt64(), nb.asInt64())).rawBits();
    } else {
        out = Value::fromDouble(float_op(asDouble(na), asDouble(nb))).rawBits();
    }
    return true;
}

// luaT_objtypename: a table's or userdata's __name, else its type's name.
std::string objTypeName(RuntimeState& rt, Value v) {
    if (v.isTable() || v.isFullUserdata()) {
        Value name = lookupMetamethod(rt, v, "__name");
        if (name.isString()) return std::string(valueToStringView(rt, name));
    }
    return typeNameOf(v);
}

// luaG_opinterror / luaG_tointerror / luaG_concaterror: the operand blamed
// is b when it is the wrong one, else a.
void throwBinOpError(RuntimeState& rt, Value a, Value b, BinOpKind kind) {
    switch (kind) {
        case BinOpKind::Concat:
            throwTypeError(rt, (a.isString() || a.isNumeric()) ? b : a, "concatenate");
        case BinOpKind::Bitwise:
            if (a.isNumeric() && b.isNumeric()) throw LuaRuntimeError("number has no integer representation");
            throwTypeError(rt, b.isNumeric() ? a : b, "perform bitwise operation on");
        case BinOpKind::Arith:
            break;
    }
    throwTypeError(rt, b.isNumeric() ? a : b, "perform arithmetic on");
}

// luaG_ordererror.
[[noreturn]] void throwOrderError(RuntimeState& rt, Value a, Value b) {
    const std::string t1 = objTypeName(rt, a);
    const std::string t2 = objTypeName(rt, b);
    if (t1 == t2) throw LuaRuntimeError("attempt to compare two " + t1 + " values");
    throw LuaRuntimeError("attempt to compare " + t1 + " with " + t2);
}

int64_t wrapAdd(int64_t x, int64_t y) noexcept {
    return static_cast<int64_t>(static_cast<uint64_t>(x) + static_cast<uint64_t>(y));
}
int64_t wrapSub(int64_t x, int64_t y) noexcept {
    return static_cast<int64_t>(static_cast<uint64_t>(x) - static_cast<uint64_t>(y));
}
int64_t wrapMul(int64_t x, int64_t y) noexcept {
    return static_cast<int64_t>(static_cast<uint64_t>(x) * static_cast<uint64_t>(y));
}

} // namespace

void throwTypeError(RuntimeState& rt, Value v, const char* op) {
    throw LuaRuntimeError(std::string("attempt to ") + op + " a " + objTypeName(rt, v) + " value");
}

} // namespace brasslua

extern "C" {

using namespace brasslua;

// A global the inline probe of the env table did not find (or a write it
// could not make): the field of _G with its metamethods, a missing one asked
// of the host.
uint64_t brasslua_rt_get_global(uint32_t intern_id) {
    auto& rt = RuntimeState::current();
    uint64_t v = brasslua_rt_table_get(Value::fromTable(rt.envTable()).rawBits(),
                                       Value::fromString(rt.getLuaStringById(intern_id)).rawBits());
    if (Value(v).isNil() && rt.hostBridge().getGlobal) v = rt.hostBridge().getGlobal(rt.getStringById(intern_id));
    return v;
}

void brasslua_rt_set_global(uint32_t intern_id, uint64_t val) {
    auto& rt = RuntimeState::current();
    brasslua_rt_table_set(Value::fromTable(rt.envTable()).rawBits(),
                          Value::fromString(rt.getLuaStringById(intern_id)).rawBits(), val);
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
    return callBinaryMetamethod(rt, a, b, "__concat", BinOpKind::Concat);
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
    throwTypeError(rt, val, "get length of");
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
    throwTypeError(rt, Value(callee), "call");
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
        throw LuaRuntimeError("attempt to perform 'n//0'");
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

int64_t brasslua_rt_forprep(uint64_t init_bits, uint64_t limit_bits, uint64_t step_bits, uint64_t* out) {
    RuntimeState& rt = RuntimeState::current();
    auto forError = [&](Value v, const char* what) {
        throw LuaRuntimeError(std::string("bad 'for' ") + what + " (number expected, got " +
                              objTypeName(rt, v) + ")");
    };
    const Value init(init_bits), limit(limit_bits), step(step_bits);
    if (init.isInteger() && step.isInteger()) {
        const int64_t i0 = init.asInt64();
        const int64_t st = step.asInt64();
        if (st == 0) throw LuaRuntimeError("'for' step is zero");
        // forlimit: the limit as an integer, floored (or ceiled for a
        // negative step); a float beyond the integers clips or skips.
        Value lim;
        if (!arithOperand(limit, lim)) forError(limit, "limit");
        int64_t l = 0;
        if (lim.isInteger()) {
            l = lim.asInt64();
        } else {
            const double f = st < 0 ? std::ceil(lim.asDouble()) : std::floor(lim.asDouble());
            if (f >= -9223372036854775808.0 && f < 9223372036854775808.0) {
                l = static_cast<int64_t>(f);
            } else if (f > 0) {
                if (st < 0) return 0;
                l = std::numeric_limits<int64_t>::max();
            } else {
                // NaN lands here too: it is not greater than 0.
                if (st > 0) return 0;
                l = std::numeric_limits<int64_t>::min();
            }
        }
        if (st > 0 ? i0 > l : i0 < l) return 0;
        out[0] = static_cast<uint64_t>(i0);
        out[1] = static_cast<uint64_t>(l);
        out[2] = static_cast<uint64_t>(st);
        return 1;
    }
    Value nl, ns, ni;
    if (!arithOperand(limit, nl)) forError(limit, "limit");
    if (!arithOperand(step, ns)) forError(step, "step");
    if (!arithOperand(init, ni)) forError(init, "initial value");
    const double fl = asDouble(nl), fs = asDouble(ns), fi = asDouble(ni);
    if (fs == 0) throw LuaRuntimeError("'for' step is zero");
    if (0 < fs ? fl < fi : fi < fl) return 0;
    std::memcpy(&out[0], &fi, 8);
    std::memcpy(&out[1], &fl, 8);
    std::memcpy(&out[2], &fs, 8);
    return 2;
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
    uint64_t r = 0;
    if (arith(a, b, wrapAdd, [](double x, double y) { return x + y; }, r)) return r;
    return callBinaryMetamethod(RuntimeState::current(), a, b, "__add", BinOpKind::Arith);
}

uint64_t brasslua_rt_sub(uint64_t a, uint64_t b) {
    Value va(a), vb(b);
    if (va.isInteger() && vb.isInteger()) {
        int64_t res = static_cast<int64_t>(static_cast<uint64_t>(va.asInt64()) -
                                           static_cast<uint64_t>(vb.asInt64()));
        return Value::fromInt64(res).rawBits();
    }
    uint64_t r = 0;
    if (arith(a, b, wrapSub, [](double x, double y) { return x - y; }, r)) return r;
    return callBinaryMetamethod(RuntimeState::current(), a, b, "__sub", BinOpKind::Arith);
}

uint64_t brasslua_rt_mul(uint64_t a, uint64_t b) {
    Value va(a), vb(b);
    if (va.isInteger() && vb.isInteger()) {
        int64_t res = static_cast<int64_t>(static_cast<uint64_t>(va.asInt64()) *
                                           static_cast<uint64_t>(vb.asInt64()));
        return Value::fromInt64(res).rawBits();
    }
    uint64_t r = 0;
    if (arith(a, b, wrapMul, [](double x, double y) { return x * y; }, r)) return r;
    return callBinaryMetamethod(RuntimeState::current(), a, b, "__mul", BinOpKind::Arith);
}

uint64_t brasslua_rt_div(uint64_t a, uint64_t b) {
    double da = 0.0, db = 0.0;
    if (tryCoerceToDouble(Value(a), da) && tryCoerceToDouble(Value(b), db)) {
        return Value::fromDouble(da / db).rawBits();
    }
    return callBinaryMetamethod(RuntimeState::current(), a, b, "__div", BinOpKind::Arith);
}

uint64_t brasslua_rt_idiv(uint64_t a, uint64_t b) {
    Value va(a), vb(b);
    if (va.isInteger() && vb.isInteger()) {
        return Value::fromInt64(brasslua_rt_idiv_i64(va.asInt64(), vb.asInt64())).rawBits();
    }
    uint64_t r = 0;
    if (arith(a, b, brasslua_rt_idiv_i64, [](double x, double y) { return std::floor(x / y); }, r)) return r;
    return callBinaryMetamethod(RuntimeState::current(), a, b, "__idiv", BinOpKind::Arith);
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
    uint64_t r = 0;
    if (arith(a, b, brasslua_rt_mod_i64, brasslua_rt_fmod, r)) return r;
    return callBinaryMetamethod(RuntimeState::current(), a, b, "__mod", BinOpKind::Arith);
}

double brasslua_rt_pow_f64(double a, double b) {
    return std::pow(a, b);
}

uint64_t brasslua_rt_pow(uint64_t a, uint64_t b) {
    double da = 0.0, db = 0.0;
    if (tryCoerceToDouble(Value(a), da) && tryCoerceToDouble(Value(b), db)) {
        return Value::fromDouble(std::pow(da, db)).rawBits();
    }
    return callBinaryMetamethod(RuntimeState::current(), a, b, "__pow", BinOpKind::Arith);
}

uint64_t brasslua_rt_unm(uint64_t a) {
    Value va(a);
    if (va.isInteger()) {
        int64_t neg = static_cast<int64_t>(0ULL - static_cast<uint64_t>(va.asInt64()));
        return Value::fromInt64(neg).rawBits();
    }
    Value n;
    if (arithOperand(va, n)) {
        if (n.isInteger()) return brasslua_rt_unm(n.rawBits());
        return Value::fromDouble(-n.asDouble()).rawBits();
    }
    auto& rt = RuntimeState::current();
    Value mm = lookupMetamethod(rt, va, "__unm");
    if (!mm.isNil()) {
        return brasslua_rt_call_1(mm.rawBits(), a);
    }
    throwBinOpError(rt, va, va, BinOpKind::Arith);
}

uint64_t brasslua_rt_band(uint64_t a, uint64_t b) {
    int64_t ia = 0, ib = 0;
    if (tryCoerceToInt64(Value(a), ia) && tryCoerceToInt64(Value(b), ib)) {
        return Value::fromInt64(ia & ib).rawBits();
    }
    return callBinaryMetamethod(RuntimeState::current(), a, b, "__band", BinOpKind::Bitwise);
}

uint64_t brasslua_rt_bor(uint64_t a, uint64_t b) {
    int64_t ia = 0, ib = 0;
    if (tryCoerceToInt64(Value(a), ia) && tryCoerceToInt64(Value(b), ib)) {
        return Value::fromInt64(ia | ib).rawBits();
    }
    return callBinaryMetamethod(RuntimeState::current(), a, b, "__bor", BinOpKind::Bitwise);
}

uint64_t brasslua_rt_bxor(uint64_t a, uint64_t b) {
    int64_t ia = 0, ib = 0;
    if (tryCoerceToInt64(Value(a), ia) && tryCoerceToInt64(Value(b), ib)) {
        return Value::fromInt64(ia ^ ib).rawBits();
    }
    return callBinaryMetamethod(RuntimeState::current(), a, b, "__bxor", BinOpKind::Bitwise);
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
    throwBinOpError(rt, Value(a), Value(a), BinOpKind::Bitwise);
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
    return callBinaryMetamethod(RuntimeState::current(), a, b, "__shl", BinOpKind::Bitwise);
}

uint64_t brasslua_rt_shr(uint64_t a, uint64_t b) {
    int64_t ia = 0, ib = 0;
    if (tryCoerceToInt64(Value(a), ia) && tryCoerceToInt64(Value(b), ib)) {
        int64_t neg_b = (ib == std::numeric_limits<int64_t>::min()) ? 64 : -ib;
        return Value::fromInt64(luaShiftLeft(ia, neg_b)).rawBits();
    }
    return callBinaryMetamethod(RuntimeState::current(), a, b, "__shr", BinOpKind::Bitwise);
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
        if (!sa || !sb || sa->length != sb->length) return 0;
        // Short strings are interned: two of them are equal only as one.
        if (sa->length <= kShortStringMax) return 0;
        if (sa->intern_id != kNotInterned && sb->intern_id != kNotInterned) return 0;
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
    throwOrderError(rt, va, vb);
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
    throwOrderError(rt, va, vb);
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
