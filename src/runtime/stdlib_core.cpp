#include "runtime/runtime_internal.h"

#include <brass/gc/fiber.hpp>
#include <brass/gc/heap.hpp>
#include <brass/gc/tracer.hpp>

#include <cctype>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace brasslua {

namespace {

// A coroutine's state off the GC heap: the brass fiber it runs on (its own
// machine stack, which brass's collector walks while it is suspended), the
// values a resume or yield hands across, and how it ended. The coroutine
// object (a GC object, which may move) is reached through `self`, traced, so
// code on either side of a switch reads it fresh after the switch rather
// than through a pointer taken before it.
struct CoroutineContext {
    // A Lua call is a native frame here, so a coroutine's recursion depth is
    // its stack's; reserved, not committed, until used.
    static constexpr size_t kStackBytes = 1024 * 1024;
    std::unique_ptr<brass::gc::Fiber> fiber;
    Value self = Value::nil();
    std::vector<uint64_t> transfer_vals;
    std::string error_msg;
    Value error_val = Value::nil();
    bool error_occurred = false;
    // Set by coroutine.close: the suspended yield the coroutine resumes in
    // throws CoroutineUnwind instead of returning.
    bool closing = false;

    LuaCoroutine* coroutine() const noexcept { return static_cast<LuaCoroutine*>(self.asPointer()); }
};

// What coroutine.close throws at a suspended coroutine's yield to unwind its
// frames. Not a Lua error and not a std::exception, so no pcall on the
// coroutine's stack catches it; the coroutine's entry does.
struct CoroutineUnwind {};

void traceCoroutineContext(void* ctx_ptr, void* tracer_ptr) {
    auto* ctx = static_cast<CoroutineContext*>(ctx_ptr);
    auto& t = *static_cast<brass::gc::Tracer*>(tracer_ptr);
    for (uint64_t& tv : ctx->transfer_vals) {
        t.visit(&tv);
    }
    t.visit(reinterpret_cast<uint64_t*>(&ctx->self));
    t.visit(reinterpret_cast<uint64_t*>(&ctx->error_val));
}

// The fiber's entry: the coroutine's body, with everything it can throw
// caught here, its outermost frame.
void coroutineEntry(void* arg) {
    auto* ctx = static_cast<CoroutineContext*>(arg);
    auto& rt = RuntimeState::current();
    try {
        std::vector<uint64_t> init_args = std::move(ctx->transfer_vals);
        ctx->transfer_vals.clear();
        uint64_t r0 = brasslua_rt_call(
            Value::fromFunction(ctx->coroutine()->entry_closure).rawBits(),
            init_args.data(),
            static_cast<int64_t>(init_args.size()));
        const auto& mret = rt.multiRetVec();
        if (!mret.empty()) {
            ctx->transfer_vals = mret;
        } else {
            ctx->transfer_vals = {r0};
        }
    } catch (const CoroutineUnwind&) {
        ctx->transfer_vals.clear();
    } catch (const LuaRuntimeError& e) {
        ctx->error_occurred = true;
        ctx->error_msg = e.what();
        ctx->error_val = e.error_value;
    } catch (const std::exception& e) {
        ctx->error_occurred = true;
        ctx->error_msg = e.what();
    } catch (...) {
        ctx->error_occurred = true;
        ctx->error_msg = "unknown error in coroutine";
    }
    ctx->coroutine()->status = CoroutineStatus::Dead;
}

CoroutineContext* contextOf(RuntimeState& rt, LuaCoroutine* co) {
    if (!co->ctx_state) {
        auto* ctx = new CoroutineContext();
        co->ctx_state = ctx;
        co->trace_ctx = &traceCoroutineContext;
        rt.heap().add_finalizer(
            reinterpret_cast<uintptr_t>(co),
            [](void* p) { delete static_cast<CoroutineContext*>(p); },
            ctx);
    }
    return static_cast<CoroutineContext*>(co->ctx_state);
}

// Runs `co` on its fiber until it yields, returns, errs or (closing) unwinds;
// `vals` go across as the resume's arguments. Returns with the coroutine's
// status and the running coroutine set back for the resumer.
void runCoroutine(RuntimeState& rt, Value co_val, CoroutineContext* ctx, std::vector<uint64_t> vals) {
    auto* co = static_cast<LuaCoroutine*>(co_val.asPointer());
    ctx->self = co_val;
    if (!ctx->fiber) {
        ctx->fiber = brass::gc::Fiber::create(&coroutineEntry, ctx, CoroutineContext::kStackBytes);
        if (!ctx->fiber) throw LuaRuntimeError("cannot create a coroutine stack");
    }
    ctx->transfer_vals = std::move(vals);

    LuaCoroutine* prev = getRunningCoroutine(rt);
    if (prev) prev->status = CoroutineStatus::Normal;
    co->caller = prev;
    co->status = CoroutineStatus::Running;
    setRunningCoroutine(rt, co);

    ctx->fiber->resume();

    // Both objects may have moved while the coroutine ran: read them again
    // through the traced references.
    co = ctx->coroutine();
    prev = co->caller;
    co->caller = nullptr;
    setRunningCoroutine(rt, prev);
    if (prev) prev->status = CoroutineStatus::Running;
    if (ctx->fiber->finished()) {
        co->status = CoroutineStatus::Dead;
        ctx->fiber.reset();
    }
}

uint64_t luaPrint(void* /*cl*/, const uint64_t* args, int64_t nargs) {
    auto& rt = RuntimeState::current();
    std::string out;
    for (int64_t i = 0; i < nargs; ++i) {
        if (i > 0) out.push_back('\t');
        out.append(valueToStdString(rt, Value(args[i])));
    }
    out.push_back('\n');
    rt.writeOutput(out);
    return kNilBits;
}

uint64_t luaType(void* /*cl*/, const uint64_t* args, int64_t nargs) {
    auto& rt = RuntimeState::current();
    if (nargs < 1) {
        throw LuaRuntimeError("bad argument #1 to 'type' (value expected)");
    }
    Value v(args[0]);
    if (v.isNil()) return rt.makeStringValue("nil");
    if (v.isBool()) return rt.makeStringValue("boolean");
    if (v.isNumeric()) return rt.makeStringValue("number");
    if (v.isString()) return rt.makeStringValue("string");
    if (v.isTable()) return rt.makeStringValue("table");
    if (v.isFunction()) return rt.makeStringValue("function");
    if (v.isCoroutine()) return rt.makeStringValue("thread");
    if (v.isUserdata()) return rt.makeStringValue("userdata");
    return rt.makeStringValue("nil");
}

uint64_t luaToString(void* /*cl*/, const uint64_t* args, int64_t nargs) {
    auto& rt = RuntimeState::current();
    Value v = nargs > 0 ? Value(args[0]) : Value::nil();
    if (v.isString()) return v.rawBits();
    return rt.makeStringValue(valueToStdString(rt, v));
}

uint64_t luaToNumber(void* /*cl*/, const uint64_t* args, int64_t nargs) {
    if (nargs < 1) return kNilBits;
    Value v(args[0]);
    int64_t base = (nargs >= 2 && !Value(args[1]).isNil()) ? Value(args[1]).asInt64() : 10;
    if (nargs < 2 || Value(args[1]).isNil()) {
        if (v.isNumeric()) return v.rawBits();
        if (v.isString()) {
            auto* s = static_cast<const LuaString*>(v.asPointer());
            if (!s || s->length == 0) return kNilBits;
            // Try integer first (Lua 5.4 tonumber returns integer when string has no dot/exponent)
            std::string_view sv(s->data, s->length);
            bool has_float_chars = false;
            bool is_hex = false;
            size_t p = 0;
            while (p < sv.size() && std::isspace(static_cast<unsigned char>(sv[p]))) ++p;
            if (p < sv.size() && (sv[p] == '+' || sv[p] == '-')) ++p;
            if (p + 1 < sv.size() && sv[p] == '0' && (sv[p + 1] == 'x' || sv[p + 1] == 'X')) {
                is_hex = true;
            }
            for (size_t i = p; i < sv.size(); ++i) {
                char c = sv[i];
                if (c == '.' || (!is_hex && (c == 'e' || c == 'E')) || (is_hex && (c == 'p' || c == 'P'))) {
                    has_float_chars = true;
                    break;
                }
            }
            if (!has_float_chars) {
                char* end = nullptr;
                long long iv = std::strtoll(s->data, &end, 0);
                while (end && *end != '\0' && std::isspace(static_cast<unsigned char>(*end))) ++end;
                if (end == s->data + s->length) {
                    return Value::fromInt64(static_cast<int64_t>(iv)).rawBits();
                }
            }
            double d = 0.0;
            if (tryCoerceToDouble(v, d)) {
                return Value::fromDouble(d).rawBits();
            }
        }
        return kNilBits;
    }
    if (!v.isString()) return kNilBits;
    if (base < 2 || base > 36) {
        throw LuaRuntimeError("bad argument #2 to 'tonumber' (base out of range)");
    }
    auto* s = static_cast<const LuaString*>(v.asPointer());
    if (!s || s->length == 0) return kNilBits;
    char* end = nullptr;
    long long val = std::strtoll(s->data, &end, static_cast<int>(base));
    while (end && *end != '\0' && std::isspace(static_cast<unsigned char>(*end))) ++end;
    if (end == s->data + s->length) {
        return Value::fromInt64(static_cast<int64_t>(val)).rawBits();
    }
    return kNilBits;
}

uint64_t luaAssert(void* /*cl*/, const uint64_t* args, int64_t nargs) {
    auto& rt = RuntimeState::current();
    Value cond = nargs > 0 ? Value(args[0]) : Value::nil();
    if (!cond.isTruthy()) {
        std::string msg = "assertion failed!";
        Value err_val = Value::nil();
        if (nargs >= 2 && !Value(args[1]).isNil()) {
            err_val = Value(args[1]);
            msg = valueToStdString(rt, err_val);
        }
        throw LuaRuntimeError(msg, err_val);
    }
    std::vector<uint64_t> ret(args, args + nargs);
    return returnMulti(rt, ret);
}

uint64_t luaError(void* /*cl*/, const uint64_t* args, int64_t nargs) {
    auto& rt = RuntimeState::current();
    Value err_val = nargs > 0 ? Value(args[0]) : Value::nil();
    throw LuaRuntimeError(valueToStdString(rt, err_val), err_val);
}

uint64_t luaPcall(void* /*cl*/, const uint64_t* args, int64_t nargs) {
    auto& rt = RuntimeState::current();
    if (nargs < 1) {
        return returnMulti(rt, {Value::fromBool(false).rawBits(), rt.makeStringValue("value expected")});
    }
    try {
        uint64_t r0 = brasslua_rt_call(args[0], nargs > 1 ? args + 1 : nullptr, nargs - 1);
        std::vector<uint64_t> ret;
        size_t mcount = rt.multiRetCount();
        ret.reserve(mcount + 1);
        ret.push_back(Value::fromBool(true).rawBits());
        if (mcount > 0) {
            for (size_t i = 0; i < mcount; ++i) {
                ret.push_back(rt.getMultiRet(i));
            }
        } else {
            ret.push_back(r0);
        }
        return returnMulti(rt, ret);
    } catch (const LuaRuntimeError& e) {
        uint64_t err_bits = !e.error_value.isNil() ? e.error_value.rawBits() : rt.makeStringValue(e.what());
        return returnMulti(rt, {Value::fromBool(false).rawBits(), err_bits});
    } catch (const std::exception& e) {
        return returnMulti(rt, {Value::fromBool(false).rawBits(), rt.makeStringValue(e.what())});
    }
}

uint64_t luaXpcall(void* /*cl*/, const uint64_t* args, int64_t nargs) {
    auto& rt = RuntimeState::current();
    if (nargs < 2) {
        return returnMulti(rt, {Value::fromBool(false).rawBits(), rt.makeStringValue("value expected")});
    }
    uint64_t fn = args[0];
    uint64_t msgh = args[1];
    try {
        uint64_t r0 = brasslua_rt_call(fn, nargs > 2 ? args + 2 : nullptr, nargs - 2);
        std::vector<uint64_t> ret;
        size_t mcount = rt.multiRetCount();
        ret.reserve(mcount + 1);
        ret.push_back(Value::fromBool(true).rawBits());
        if (mcount > 0) {
            for (size_t i = 0; i < mcount; ++i) {
                ret.push_back(rt.getMultiRet(i));
            }
        } else {
            ret.push_back(r0);
        }
        return returnMulti(rt, ret);
    } catch (const LuaRuntimeError& e) {
        uint64_t err_bits = !e.error_value.isNil() ? e.error_value.rawBits() : rt.makeStringValue(e.what());
        try {
            uint64_t handled = brasslua_rt_call_1(msgh, err_bits);
            return returnMulti(rt, {Value::fromBool(false).rawBits(), handled});
        } catch (...) {
            return returnMulti(rt, {Value::fromBool(false).rawBits(), rt.makeStringValue("error in error handling")});
        }
    } catch (const std::exception& e) {
        uint64_t err_bits = rt.makeStringValue(e.what());
        try {
            uint64_t handled = brasslua_rt_call_1(msgh, err_bits);
            return returnMulti(rt, {Value::fromBool(false).rawBits(), handled});
        } catch (...) {
            return returnMulti(rt, {Value::fromBool(false).rawBits(), rt.makeStringValue("error in error handling")});
        }
    }
}

uint64_t luaSelect(void* /*cl*/, const uint64_t* args, int64_t nargs) {
    auto& rt = RuntimeState::current();
    if (nargs < 1) return kNilBits;
    Value first(args[0]);
    if (first.isString() && valueToStringView(rt, first) == "#") {
        return Value::fromInt64(nargs - 1).rawBits();
    }
    int64_t idx = first.asInt64();
    int64_t total = nargs - 1;
    if (idx < 0) {
        idx = total + idx + 1;
    }
    if (idx < 1) idx = 1;
    std::vector<uint64_t> ret;
    for (int64_t i = idx; i <= total; ++i) {
        ret.push_back(args[i]);
    }
    return returnMulti(rt, ret);
}

uint64_t luaUnpack(void* /*cl*/, const uint64_t* args, int64_t nargs) {
    auto& rt = RuntimeState::current();
    if (nargs < 1 || !Value(args[0]).isTable()) return kNilBits;
    uint64_t tbl_bits = args[0];
    int64_t i = (nargs >= 2 && !Value(args[1]).isNil()) ? Value(args[1]).asInt64() : 1;
    int64_t j = (nargs >= 3 && !Value(args[2]).isNil()) ? Value(args[2]).asInt64() : brasslua_rt_table_len(tbl_bits);
    std::vector<uint64_t> ret;
    if (j >= i) {
        ret.reserve(static_cast<size_t>(j - i + 1));
        for (int64_t k = i; k <= j; ++k) {
            ret.push_back(brasslua_rt_table_get_int(tbl_bits, k));
        }
    }
    return returnMulti(rt, ret);
}

uint64_t luaRawget(void* /*cl*/, const uint64_t* args, int64_t nargs) {
    if (nargs < 2) return kNilBits;
    return brasslua_rt_rawget(args[0], args[1]);
}

uint64_t luaRawset(void* /*cl*/, const uint64_t* args, int64_t nargs) {
    if (nargs < 3) return kNilBits;
    return brasslua_rt_rawset(args[0], args[1], args[2]);
}

uint64_t luaRawequal(void* /*cl*/, const uint64_t* args, int64_t nargs) {
    if (nargs < 2) return Value::fromBool(false).rawBits();
    Value a(args[0]), b(args[1]);
    if (a.isNumeric() && b.isNumeric()) {
        if (a.isInteger() && b.isInteger()) {
            return Value::fromBool(a.asInt64() == b.asInt64()).rawBits();
        }
        if (a.isFloat() && b.isFloat()) {
            return Value::fromBool(a.asDouble() == b.asDouble()).rawBits();
        }
        return Value::fromBool(brasslua_rt_eq(args[0], args[1]) != 0).rawBits();
    }
    return Value::fromBool(a == b).rawBits();
}

uint64_t luaRawlen(void* /*cl*/, const uint64_t* args, int64_t nargs) {
    if (nargs < 1) return Value::fromInt64(0).rawBits();
    Value v(args[0]);
    if (v.isString()) {
        auto* s = static_cast<const LuaString*>(v.asPointer());
        return Value::fromInt64(s ? static_cast<int64_t>(s->length) : 0).rawBits();
    }
    if (v.isTable()) {
        auto* tbl = static_cast<const LuaTable*>(v.asPointer());
        uint32_t len = 0;
        while (len < tbl->array_cap && !Value(tbl->array_data[len]).isNil()) {
            ++len;
        }
        return Value::fromInt64(static_cast<int64_t>(len)).rawBits();
    }
    throw LuaRuntimeError("bad argument #1 to 'rawlen' (table or string expected)");
}

uint64_t luaGetmetatable(void* /*cl*/, const uint64_t* args, int64_t nargs) {
    if (nargs < 1) return kNilBits;
    auto& rt = RuntimeState::current();
    Value v(args[0]);
    LuaTable* mt = nullptr;
    if (v.isTable()) {
        auto* tbl = static_cast<LuaTable*>(v.asPointer());
        if (tbl) mt = tbl->metatable;
    } else if (v.isFullUserdata()) {
        auto* ud = static_cast<LuaUserdata*>(v.asPointer());
        if (ud) mt = ud->metatable;
    } else if (v.isString()) {
        mt = getStringMetatable(rt);
    }
    if (!mt) return kNilBits;
    uint32_t mm_id = rt.internString("__metatable");
    uint64_t protected_mt = brasslua_rt_rawget(
        Value::fromTable(mt).rawBits(),
        Value::fromString(rt.getLuaStringById(mm_id)).rawBits());
    if (!Value(protected_mt).isNil()) {
        return protected_mt;
    }
    return Value::fromTable(mt).rawBits();
}

uint64_t luaSetmetatable(void* /*cl*/, const uint64_t* args, int64_t nargs) {
    if (nargs < 2 || !Value(args[0]).isTable()) {
        throw LuaRuntimeError("bad argument #1 to 'setmetatable' (table expected)");
    }
    auto& rt = RuntimeState::current();
    auto* tbl = static_cast<LuaTable*>(Value(args[0]).asPointer());
    if (tbl->metatable) {
        uint32_t mm_id = rt.internString("__metatable");
        uint64_t protected_mt = brasslua_rt_rawget(
            Value::fromTable(tbl->metatable).rawBits(),
            Value::fromString(rt.getLuaStringById(mm_id)).rawBits());
        if (!Value(protected_mt).isNil()) {
            throw LuaRuntimeError("cannot change a protected metatable");
        }
    }
    Value mt_val(args[1]);
    if (mt_val.isNil()) {
        tbl->metatable = nullptr;
        tbl->flags = 0;
    } else if (mt_val.isTable()) {
        tbl->metatable = static_cast<LuaTable*>(mt_val.asPointer());
        recomputeMetatableFlags(rt, tbl);
        rt.heap().remember(reinterpret_cast<uintptr_t>(tbl));
    } else {
        throw LuaRuntimeError("bad argument #2 to 'setmetatable' (nil or table expected)");
    }
    return args[0];
}

uint64_t luaNext(void* /*cl*/, const uint64_t* args, int64_t nargs) {
    auto& rt = RuntimeState::current();
    if (nargs < 1 || !Value(args[0]).isTable()) return kNilBits;
    uint64_t key = nargs >= 2 ? args[1] : kNilBits;
    uint64_t val = kNilBits;
    uint64_t next_key = brasslua_rt_next(args[0], key, &val);
    if (Value(next_key).isNil()) {
        return returnMulti(rt, {kNilBits});
    }
    return returnMulti(rt, {next_key, val});
}

uint64_t luaPairs(void* /*cl*/, const uint64_t* args, int64_t nargs) {
    auto& rt = RuntimeState::current();
    if (nargs < 1) return kNilBits;
    Value t(args[0]);
    Value mm = lookupMetamethod(rt, t, "__pairs");
    if (!mm.isNil()) {
        return brasslua_rt_call_1(mm.rawBits(), args[0]);
    }
    Value next_fn = rt.getGlobal("next");
    return returnMulti(rt, {next_fn.rawBits(), args[0], kNilBits});
}

uint64_t luaIpairsIter(void* /*cl*/, const uint64_t* args, int64_t nargs) {
    auto& rt = RuntimeState::current();
    if (nargs < 2) return kNilBits;
    int64_t idx = Value(args[1]).asInt64() + 1;
    uint64_t v = brasslua_rt_table_get_int(args[0], idx);
    if (Value(v).isNil()) {
        return returnMulti(rt, {kNilBits});
    }
    return returnMulti(rt, {Value::fromInt64(idx).rawBits(), v});
}

uint64_t luaIpairs(void* /*cl*/, const uint64_t* args, int64_t nargs) {
    auto& rt = RuntimeState::current();
    if (nargs < 1) return kNilBits;
    Value iter_fn = makeNativeClosure(rt, &luaIpairsIter, 2);
    return returnMulti(rt, {iter_fn.rawBits(), args[0], Value::fromInt64(0).rawBits()});
}

uint64_t luaCollectgarbage(void* /*cl*/, const uint64_t* args, int64_t nargs) {
    auto& rt = RuntimeState::current();
    std::string_view opt = (nargs >= 1 && Value(args[0]).isString())
                               ? valueToStringView(rt, Value(args[0]))
                               : "collect";
    if (opt == "count") {
        double kb = static_cast<double>(rt.heap().young_used_bytes() + rt.heap().old_used_bytes()) / 1024.0;
        return Value::fromDouble(kb).rawBits();
    }
    if (opt == "isrunning") {
        return Value::fromBool(true).rawBits();
    }
    if (opt == "collect" || opt == "step") {
        rt.collectGarbage();
        return Value::fromBool(true).rawBits();
    }
    return Value::fromInt64(0).rawBits();
}

uint64_t luaLoadChunkRunner(void* cl_ptr, const uint64_t* /*args*/, int64_t /*nargs*/) {
    auto& rt = RuntimeState::current();
    auto* cl = static_cast<LuaClosure*>(cl_ptr);
    if (!cl || cl->upvalue_count < 1 || !cl->upvalues[0]) return kNilBits;
    Value code_val(cl->upvalues[0]->value);
    std::string_view src = valueToStringView(rt, code_val);
    ExecutionResult res = evalLuaSource(src);
    if (!res.success) {
        throw LuaRuntimeError(res.error_message);
    }
    if (!res.return_values.empty()) {
        std::vector<uint64_t> rbits;
        rbits.reserve(res.return_values.size());
        for (const Value& v : res.return_values) rbits.push_back(v.rawBits());
        return returnMulti(rt, rbits);
    }
    return res.return_value.rawBits();
}

uint64_t luaLoad(void* /*cl*/, const uint64_t* args, int64_t nargs) {
    auto& rt = RuntimeState::current();
    if (nargs < 1) return returnMulti(rt, {kNilBits, rt.makeStringValue("expected string or function")});
    std::string code;
    if (Value(args[0]).isString()) {
        code = std::string(valueToStringView(rt, Value(args[0])));
    } else if (Value(args[0]).isFunction()) {
        for (;;) {
            uint64_t piece = brasslua_rt_call_0(args[0]);
            if (Value(piece).isNil()) break;
            std::string_view sv = valueToStringView(rt, Value(piece));
            if (sv.empty()) break;
            code.append(sv);
        }
    } else {
        return returnMulti(rt, {kNilBits, rt.makeStringValue("expected string or reader function")});
    }
    try {
        (void)parseLuaSource(code, "=load");
    } catch (const std::exception& e) {
        return returnMulti(rt, {kNilBits, rt.makeStringValue(e.what())});
    }
    LuaClosure* runner = rt.allocClosure(&luaLoadChunkRunner, nullptr, 1, 0);
    runner->upvalues[0]->value = rt.makeStringValue(code);
    return Value::fromFunction(runner).rawBits();
}

uint64_t luaDofile(void* /*cl*/, const uint64_t* args, int64_t nargs) {
    auto& rt = RuntimeState::current();
    if (nargs < 1 || !Value(args[0]).isString()) {
        throw LuaRuntimeError("bad argument #1 to 'dofile' (string expected)");
    }
    std::string path(valueToStringView(rt, Value(args[0])));
    ExecutionResult res = evalLuaFile(path);
    if (!res.success) {
        throw LuaRuntimeError(res.error_message);
    }
    if (!res.return_values.empty()) {
        std::vector<uint64_t> rbits;
        rbits.reserve(res.return_values.size());
        for (const Value& v : res.return_values) rbits.push_back(v.rawBits());
        return returnMulti(rt, rbits);
    }
    return res.return_value.rawBits();
}

// ---- Coroutine Library ----

uint64_t luaCoroCreate(void* /*cl*/, const uint64_t* args, int64_t nargs) {
    if (nargs < 1 || !Value(args[0]).isFunction()) {
        throw LuaRuntimeError("bad argument #1 to 'coroutine.create' (function expected)");
    }
    auto& rt = RuntimeState::current();
    auto* entry = static_cast<LuaClosure*>(Value(args[0]).asPointer());
    LuaCoroutine* co = rt.allocCoroutine(entry);
    return Value::fromCoroutine(co).rawBits();
}

uint64_t luaCoroStatus(void* /*cl*/, const uint64_t* args, int64_t nargs) {
    if (nargs < 1 || !Value(args[0]).isCoroutine()) {
        throw LuaRuntimeError("bad argument #1 to 'coroutine.status' (thread expected)");
    }
    auto& rt = RuntimeState::current();
    auto* co = static_cast<LuaCoroutine*>(Value(args[0]).asPointer());
    switch (co->status) {
        case CoroutineStatus::Suspended: return rt.makeStringValue("suspended");
        case CoroutineStatus::Running:   return rt.makeStringValue("running");
        case CoroutineStatus::Normal:    return rt.makeStringValue("normal");
        case CoroutineStatus::Dead:      return rt.makeStringValue("dead");
    }
    return rt.makeStringValue("dead");
}

uint64_t luaCoroRunning(void* /*cl*/, const uint64_t* /*args*/, int64_t /*nargs*/) {
    auto& rt = RuntimeState::current();
    LuaCoroutine* running = getRunningCoroutine(rt);
    if (!running) {
        return returnMulti(rt, {kNilBits, Value::fromBool(true).rawBits()});
    }
    return returnMulti(rt, {Value::fromCoroutine(running).rawBits(), Value::fromBool(false).rawBits()});
}

uint64_t luaCoroIsYieldable(void* /*cl*/, const uint64_t* args, int64_t nargs) {
    auto& rt = RuntimeState::current();
    if (nargs >= 1 && Value(args[0]).isCoroutine()) {
        auto* co = static_cast<LuaCoroutine*>(Value(args[0]).asPointer());
        return Value::fromBool(co != nullptr && co->status != CoroutineStatus::Dead).rawBits();
    }
    return Value::fromBool(getRunningCoroutine(rt) != nullptr).rawBits();
}

uint64_t luaCoroResume(void* /*cl*/, const uint64_t* args, int64_t nargs) {
    if (nargs < 1 || !Value(args[0]).isCoroutine()) {
        throw LuaRuntimeError("bad argument #1 to 'coroutine.resume' (thread expected)");
    }
    auto& rt = RuntimeState::current();
    auto* co = static_cast<LuaCoroutine*>(Value(args[0]).asPointer());
    if (co->status != CoroutineStatus::Suspended) {
        return returnMulti(rt, {Value::fromBool(false).rawBits(),
                                rt.makeStringValue("cannot resume non-suspended coroutine")});
    }

    CoroutineContext* ctx = contextOf(rt, co);
    runCoroutine(rt, Value(args[0]), ctx, std::vector<uint64_t>(args + 1, args + nargs));

    if (ctx->error_occurred) {
        ctx->error_occurred = false;
        uint64_t err_bits = !ctx->error_val.isNil() ? ctx->error_val.rawBits() : rt.makeStringValue(ctx->error_msg);
        ctx->error_val = Value::nil();
        return returnMulti(rt, {Value::fromBool(false).rawBits(), err_bits});
    }
    std::vector<uint64_t> ret;
    ret.reserve(ctx->transfer_vals.size() + 1);
    ret.push_back(Value::fromBool(true).rawBits());
    ret.insert(ret.end(), ctx->transfer_vals.begin(), ctx->transfer_vals.end());
    ctx->transfer_vals.clear();
    return returnMulti(rt, ret);
}

uint64_t luaCoroYield(void* /*cl*/, const uint64_t* args, int64_t nargs) {
    auto& rt = RuntimeState::current();
    LuaCoroutine* co = getRunningCoroutine(rt);
    if (!co || !co->ctx_state || brass::gc::Fiber::current() == nullptr) {
        throw LuaRuntimeError("attempt to yield from outside a coroutine");
    }
    auto* ctx = static_cast<CoroutineContext*>(co->ctx_state);
    ctx->transfer_vals.assign(args, args + nargs);
    co->status = CoroutineStatus::Suspended;
    brass::gc::Fiber::suspend();
    if (ctx->closing) throw CoroutineUnwind{};
    std::vector<uint64_t> resumed = std::move(ctx->transfer_vals);
    ctx->transfer_vals.clear();
    return returnMulti(rt, resumed);
}

uint64_t luaCoroWrapCaller(void* cl_ptr, const uint64_t* args, int64_t nargs) {
    auto& rt = RuntimeState::current();
    auto* cl = static_cast<LuaClosure*>(cl_ptr);
    if (!cl || cl->upvalue_count < 1 || !cl->upvalues[0]) return kNilBits;
    uint64_t co_bits = cl->upvalues[0]->value;
    std::vector<uint64_t> call_args;
    call_args.reserve(static_cast<size_t>(nargs) + 1);
    call_args.push_back(co_bits);
    for (int64_t i = 0; i < nargs; ++i) call_args.push_back(args[i]);
    luaCoroResume(nullptr, call_args.data(), static_cast<int64_t>(call_args.size()));
    const auto& mret = rt.multiRetVec();
    if (mret.empty() || !Value(mret[0]).isTruthy()) {
        std::string msg = mret.size() >= 2 ? valueToStdString(rt, Value(mret[1])) : "coroutine error";
        Value ev = mret.size() >= 2 ? Value(mret[1]) : Value::nil();
        throw LuaRuntimeError(msg, ev);
    }
    std::vector<uint64_t> unwrapped(mret.begin() + 1, mret.end());
    return returnMulti(rt, unwrapped);
}

uint64_t luaCoroWrap(void* /*cl*/, const uint64_t* args, int64_t nargs) {
    uint64_t co_bits = luaCoroCreate(nullptr, args, nargs);
    auto& rt = RuntimeState::current();
    LuaClosure* wrapper = rt.allocClosure(&luaCoroWrapCaller, nullptr, 1, 0);
    wrapper->upvalues[0]->value = co_bits;
    return Value::fromFunction(wrapper).rawBits();
}

uint64_t luaCoroClose(void* /*cl*/, const uint64_t* args, int64_t nargs) {
    if (nargs < 1 || !Value(args[0]).isCoroutine()) {
        throw LuaRuntimeError("bad argument #1 to 'coroutine.close' (thread expected)");
    }
    auto& rt = RuntimeState::current();
    auto* co = static_cast<LuaCoroutine*>(Value(args[0]).asPointer());
    if (co->status == CoroutineStatus::Running || co->status == CoroutineStatus::Normal) {
        throw LuaRuntimeError("cannot close a running or normal coroutine");
    }
    // A coroutine suspended mid-body is resumed once more, to unwind: its
    // yield throws CoroutineUnwind, which runs the C++ frames' destructors on
    // the way out and ends at the entry; then its stack is freed.
    auto* ctx = static_cast<CoroutineContext*>(co->ctx_state);
    if (ctx && ctx->fiber && ctx->fiber->started() && co->status == CoroutineStatus::Suspended) {
        ctx->closing = true;
        runCoroutine(rt, Value(args[0]), ctx, {});
        ctx->closing = false;
        co = ctx->coroutine();
    }
    co->status = CoroutineStatus::Dead;
    if (ctx) {
        ctx->fiber.reset();
        ctx->transfer_vals.clear();
        if (ctx->error_occurred) {
            ctx->error_occurred = false;
            uint64_t err_bits =
                !ctx->error_val.isNil() ? ctx->error_val.rawBits() : rt.makeStringValue(ctx->error_msg);
            ctx->error_val = Value::nil();
            return returnMulti(rt, {Value::fromBool(false).rawBits(), err_bits});
        }
    }
    return Value::fromBool(true).rawBits();
}

} // namespace

void registerCoreStdlib(RuntimeState& rt) {
    rt.setGlobal("print", makeNativeClosure(rt, &luaPrint));
    rt.setGlobal("type", makeNativeClosure(rt, &luaType, 1));
    rt.setGlobal("tostring", makeNativeClosure(rt, &luaToString, 1));
    rt.setGlobal("tonumber", makeNativeClosure(rt, &luaToNumber, 1));
    rt.setGlobal("assert", makeNativeClosure(rt, &luaAssert, 1));
    rt.setGlobal("error", makeNativeClosure(rt, &luaError, 1));
    rt.setGlobal("pcall", makeNativeClosure(rt, &luaPcall, 1));
    rt.setGlobal("xpcall", makeNativeClosure(rt, &luaXpcall, 2));
    rt.setGlobal("select", makeNativeClosure(rt, &luaSelect, 1));
    rt.setGlobal("unpack", makeNativeClosure(rt, &luaUnpack, 1));
    rt.setGlobal("rawget", makeNativeClosure(rt, &luaRawget, 2));
    rt.setGlobal("rawset", makeNativeClosure(rt, &luaRawset, 3));
    rt.setGlobal("rawequal", makeNativeClosure(rt, &luaRawequal, 2));
    rt.setGlobal("rawlen", makeNativeClosure(rt, &luaRawlen, 1));
    rt.setGlobal("getmetatable", makeNativeClosure(rt, &luaGetmetatable, 1));
    rt.setGlobal("setmetatable", makeNativeClosure(rt, &luaSetmetatable, 2));
    rt.setGlobal("next", makeNativeClosure(rt, &luaNext, 2));
    rt.setGlobal("pairs", makeNativeClosure(rt, &luaPairs, 1));
    rt.setGlobal("ipairs", makeNativeClosure(rt, &luaIpairs, 1));
    rt.setGlobal("collectgarbage", makeNativeClosure(rt, &luaCollectgarbage, 1));
    rt.setGlobal("load", makeNativeClosure(rt, &luaLoad, 1));
    rt.setGlobal("loadstring", makeNativeClosure(rt, &luaLoad, 1));
    rt.setGlobal("dofile", makeNativeClosure(rt, &luaDofile, 1));
    rt.setGlobal("_VERSION", Value(rt.makeStringValue("Lua 5.4")));
    rt.setGlobal("_G", Value::fromTable(rt.envTable()));

    LuaTable* coro_tbl = rt.allocTable(0, 8);
    setTableFieldStr(rt, coro_tbl, "create", makeNativeClosure(rt, &luaCoroCreate, 1));
    setTableFieldStr(rt, coro_tbl, "resume", makeNativeClosure(rt, &luaCoroResume, 1));
    setTableFieldStr(rt, coro_tbl, "yield", makeNativeClosure(rt, &luaCoroYield, 0));
    setTableFieldStr(rt, coro_tbl, "wrap", makeNativeClosure(rt, &luaCoroWrap, 1));
    setTableFieldStr(rt, coro_tbl, "status", makeNativeClosure(rt, &luaCoroStatus, 1));
    setTableFieldStr(rt, coro_tbl, "running", makeNativeClosure(rt, &luaCoroRunning, 0));
    setTableFieldStr(rt, coro_tbl, "isyieldable", makeNativeClosure(rt, &luaCoroIsYieldable, 0));
    setTableFieldStr(rt, coro_tbl, "close", makeNativeClosure(rt, &luaCoroClose, 1));
    rt.setGlobal("coroutine", Value::fromTable(coro_tbl));
}

} // namespace brasslua
