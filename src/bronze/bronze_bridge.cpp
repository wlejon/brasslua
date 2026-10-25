#include "brasslua/bronze_bridge.h"
#include "runtime/runtime_internal.h"

#include <brass/gc/heap.hpp>

#include <cmath>
#include <cstring>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace brasslua::bronze_bridge {

namespace {

constexpr int64_t kMinSafeJsInt = -9007199254740991LL; // -(2^53 - 1)
constexpr int64_t kMaxSafeJsInt =  9007199254740991LL; // +(2^53 - 1)

// Strict RFC 3629 UTF-8 validation. Also rejects embedded NUL bytes so binary
// buffers containing NUL or non-UTF-8 bytes map cleanly to Uint8Array.
bool isValidUtf8Text(std::string_view sv) noexcept {
    const auto* bytes = reinterpret_cast<const uint8_t*>(sv.data());
    const size_t len = sv.size();
    size_t i = 0;
    while (i < len) {
        uint8_t b0 = bytes[i];
        if (b0 == 0x00) {
            return false;
        }
        if (b0 < 0x80) {
            ++i;
            continue;
        }
        if (b0 >= 0xC2 && b0 <= 0xDF) {
            if (i + 1 >= len) return false;
            uint8_t b1 = bytes[i + 1];
            if ((b1 & 0xC0) != 0x80) return false;
            i += 2;
            continue;
        }
        if (b0 >= 0xE0 && b0 <= 0xEF) {
            if (i + 2 >= len) return false;
            uint8_t b1 = bytes[i + 1];
            uint8_t b2 = bytes[i + 2];
            if ((b1 & 0xC0) != 0x80 || (b2 & 0xC0) != 0x80) return false;
            if (b0 == 0xE0 && b1 < 0xA0) return false; // overlong
            if (b0 == 0xED && b1 >= 0xA0) return false; // UTF-16 surrogate
            i += 3;
            continue;
        }
        if (b0 >= 0xF0 && b0 <= 0xF4) {
            if (i + 3 >= len) return false;
            uint8_t b1 = bytes[i + 1];
            uint8_t b2 = bytes[i + 2];
            uint8_t b3 = bytes[i + 3];
            if ((b1 & 0xC0) != 0x80 || (b2 & 0xC0) != 0x80 || (b3 & 0xC0) != 0x80) return false;
            if (b0 == 0xF0 && b1 < 0x90) return false; // overlong
            if (b0 == 0xF4 && b1 >= 0x90) return false; // > U+10FFFF
            i += 4;
            continue;
        }
        return false;
    }
    return true;
}

std::string extractJsErrorMessage(bronze::Value thrown) {
    bronze::embed::HandleScope scope;
    bronze::embed::Local<bronze::Value> err(scope, thrown);
    if (bronze::embed::isString(err.get())) {
        return bronze::embed::toUtf8(err.get());
    }
    if (bronze::embed::isObject(err.get())) {
        bronze::Value msg = bronze::embed::getProperty(err.get(), "message");
        if (bronze::embed::isString(msg)) {
            return bronze::embed::toUtf8(msg);
        }
    }
    return bronze::embed::toUtf8(err.get());
}

bool isJsArray(bronze::Value v) {
    if (!bronze::embed::isObject(v)) return false;
    bronze::embed::HandleScope scope;
    bronze::embed::Local<bronze::Value> obj(scope, v);
    auto arr_glob = bronze::embed::globalValue("Array");
    if (!arr_glob.found || !bronze::embed::isObject(arr_glob.value)) return false;
    bronze::embed::Local<bronze::Value> arr_ctor(scope, arr_glob.value);
    bronze::embed::Local<bronze::Value> is_arr_fn(
        scope, bronze::embed::getProperty(arr_ctor.get(), "isArray"));
    if (!bronze::embed::isFunction(is_arr_fn.get())) return false;
    bronze::Value arg = obj.get();
    auto res = bronze::embed::call(is_arr_fn.get(), arr_ctor.get(), std::span<const bronze::Value>(&arg, 1));
    return !res.thrown && bronze::embed::toBool(res.value);
}

// Inspects whether a LuaTable is a plain 1-based sequence table (no metatable,
// no inline record slots set, no hash entries set, and contiguous 1..N elements
// with N >= 1).
bool isPlainSequenceTable(const LuaTable* tbl, uint32_t& out_len) noexcept {
    out_len = 0;
    if (!tbl || tbl->metatable != nullptr) return false;
    for (size_t s = 0; s < LuaTable::kInlineSlots; ++s) {
        if (!Value(tbl->slots[s]).isNil()) return false;
    }
    for (uint32_t h = 0; h < tbl->hash_len; ++h) {
        if (!Value(tbl->hash_data[h].key).isNil() && !Value(tbl->hash_data[h].val).isNil()) {
            return false;
        }
    }
    uint32_t len = 0;
    while (len < tbl->array_cap && !Value(tbl->array_data[len]).isNil()) {
        ++len;
    }
    for (uint32_t i = len; i < tbl->array_cap; ++i) {
        if (!Value(tbl->array_data[i]).isNil()) {
            return false; // sparse array hole
        }
    }
    out_len = len;
    return len > 0;
}

struct AwaitState {
    RuntimeState* rt = nullptr;
    LuaCoroutine* suspended_coro = nullptr;
    bool settled = false;
    bool rejected = false;
    bronze::embed::Persistent result;
    std::string error_msg;
};

void resumeSuspendedCoroutineForAwait(const std::shared_ptr<AwaitState>& state) {
    if (!state->suspended_coro || !state->rt) return;
    LuaCoroutine* co = state->suspended_coro;
    state->suspended_coro = nullptr;
    if (co->status != CoroutineStatus::Suspended) return;

    Value coro_tbl = state->rt->getGlobal("coroutine");
    if (!coro_tbl.isTable()) return;
    uint32_t resume_id = state->rt->internString("resume");
    uint64_t resume_fn = brasslua_rt_table_get_field(
        coro_tbl.rawBits(), resume_id,
        static_cast<uint32_t>(state->rt->findFieldSlot(resume_id)));
    if (Value(resume_fn).isFunction()) {
        brasslua_rt_call_1(resume_fn, Value::fromCoroutine(co).rawBits());
    }
}

uint64_t luaAwaitPromise(void* /*cl*/, const uint64_t* args, int64_t nargs) {
    if (nargs < 1) return kNilBits;
    auto& rt = RuntimeState::current();
    Value arg(args[0]);
    if (!isBronzeHostObject(arg)) {
        return arg.rawBits();
    }

    bronze::embed::HandleScope scope;
    bronze::embed::Local<bronze::Value> promise(scope, unwrapBronzeHostObject(arg));
    if (!bronze::embed::isPromise(promise.get())) {
        return arg.rawBits();
    }

    auto state = std::make_shared<AwaitState>();
    state->rt = &rt;

    bronze::embed::Local<bronze::Value> on_fulfilled(
        scope,
        bronze::embed::makeFunction(
            [state](bronze::Value, std::span<const bronze::Value> cb_args) -> bronze::Value {
                state->settled = true;
                state->rejected = false;
                state->result.set(cb_args.empty() ? bronze::embed::undefined() : cb_args[0]);
                resumeSuspendedCoroutineForAwait(state);
                return bronze::embed::undefined();
            },
            1));

    bronze::embed::Local<bronze::Value> on_rejected(
        scope,
        bronze::embed::makeFunction(
            [state](bronze::Value, std::span<const bronze::Value> cb_args) -> bronze::Value {
                state->settled = true;
                state->rejected = true;
                bronze::Value reason = cb_args.empty() ? bronze::embed::undefined() : cb_args[0];
                state->result.set(reason);
                state->error_msg = extractJsErrorMessage(reason);
                resumeSuspendedCoroutineForAwait(state);
                return bronze::embed::undefined();
            },
            1));

    bronze::embed::Local<bronze::Value> then_fn(
        scope, bronze::embed::getProperty(promise.get(), "then"));
    if (bronze::embed::isFunction(then_fn.get())) {
        bronze::Value then_args[2] = {on_fulfilled.get(), on_rejected.get()};
        (void)bronze::embed::call(then_fn.get(), promise.get(),
                                  std::span<const bronze::Value>(then_args, 2));
    }

    bronze::embed::drainMicrotasks();

    if (!state->settled) {
        LuaCoroutine* running = getRunningCoroutine(rt);
        if (running != nullptr) {
            state->suspended_coro = running;
            Value coro_tbl = rt.getGlobal("coroutine");
            if (coro_tbl.isTable()) {
                uint32_t yield_id = rt.internString("yield");
                uint64_t yield_fn = brasslua_rt_table_get_field(
                    coro_tbl.rawBits(), yield_id,
                    static_cast<uint32_t>(rt.findFieldSlot(yield_id)));
                if (Value(yield_fn).isFunction()) {
                    brasslua_rt_call_0(yield_fn);
                }
            }
        }
    }

    if (!state->settled && bronze::embed::microtasksPending()) {
        bronze::embed::drainMicrotasks();
    }

    if (state->rejected) {
        throw LuaRuntimeError(state->error_msg.empty() ? "promise rejected" : state->error_msg);
    }
    if (!state->settled) {
        return kNilBits;
    }
    return jsToLua(rt, state->result.get()).rawBits();
}

uint64_t luaToTableHelper(void* /*cl*/, const uint64_t* args, int64_t nargs) {
    if (nargs < 1) return kNilBits;
    auto& rt = RuntimeState::current();
    Value arg(args[0]);
    if (isBronzeHostObject(arg)) {
        return jsToLua(rt, unwrapBronzeHostObject(arg), /*convertPlainContainers=*/true).rawBits();
    }
    return arg.rawBits();
}

} // namespace

bool isBronzeHostObject(Value v) noexcept {
    if (!v.isFullUserdata()) return false;
    const auto* ud = static_cast<const LuaUserdata*>(v.asPointer());
    return ud != nullptr && ud->type_tag == kBronzeHostTypeTag && ud->data != nullptr;
}

bronze::Value unwrapBronzeHostObject(Value v) noexcept {
    if (!isBronzeHostObject(v)) return bronze::embed::undefined();
    const auto* ud = static_cast<const LuaUserdata*>(v.asPointer());
    return static_cast<const bronze::embed::Persistent*>(ud->data)->get();
}

Value wrapBronzeHostObject(RuntimeState& rt, bronze::Value v) {
    bronze::embed::HandleScope scope;
    bronze::embed::Local<bronze::Value> rooted(scope, v);
    LuaUserdata* ud = rt.allocUserdata(sizeof(bronze::embed::Persistent), kBronzeHostTypeTag);
    new (ud->data) bronze::embed::Persistent(rooted.get());
    ud->finalizer = [](void* data, uint32_t) {
        if (data) {
            static_cast<bronze::embed::Persistent*>(data)->~Persistent();
        }
    };
    return Value::fromUserdata(ud);
}

bronze::Value luaToJs(RuntimeState& rt, Value v) {
    // Rule 3: nil -> undefined, null sentinel -> null
    if (v.isNil()) {
        return bronze::embed::undefined();
    }
    if (v.isNullSentinel()) {
        return bronze::embed::null();
    }
    if (v.isBool()) {
        return bronze::embed::fromBool(v.asBool());
    }
    // Rule 4: 64-bit Integers (safe range -> Number, outside -> BigInt)
    if (v.isInteger()) {
        int64_t iv = v.asInt64();
        if (iv >= kMinSafeJsInt && iv <= kMaxSafeJsInt) {
            return bronze::embed::fromDouble(static_cast<double>(iv));
        }
        bronze::embed::HandleScope scope;
        bronze::embed::Local<bronze::Value> ta(
            scope, bronze::embed::createTypedArray(bronze::embed::elements::BigInt64, 1));
        bronze::embed::fillTypedArray(
            ta.get(),
            std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(&iv), sizeof(int64_t)));
        return bronze::embed::getElement(ta.get(), 0);
    }
    if (v.isFloat()) {
        return bronze::embed::fromDouble(v.asDouble());
    }
    // Rule 1: Strings (valid UTF-8 -> JS String, binary bytes -> Uint8Array)
    if (v.isString()) {
        std::string_view sv = valueToStringView(rt, v);
        if (isValidUtf8Text(sv)) {
            return bronze::embed::fromUtf8(sv);
        }
        bronze::embed::HandleScope scope;
        bronze::embed::Local<bronze::Value> ta(
            scope, bronze::embed::createTypedArray(
                       bronze::embed::elements::Uint8, static_cast<uint32_t>(sv.size())));
        if (!sv.empty()) {
            bronze::embed::fillTypedArray(
                ta.get(),
                std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(sv.data()), sv.size()));
        }
        return ta.get();
    }
    // Unwrap if already a wrapped bronze host object
    if (isBronzeHostObject(v)) {
        return unwrapBronzeHostObject(v);
    }
    // Rule 2: Plain Lua tables -> JS Array (1-based sequence) or JS Object (record/map)
    if (v.isTable()) {
        const auto* tbl = static_cast<const LuaTable*>(v.asPointer());
        bronze::embed::HandleScope scope;
        uint32_t seq_len = 0;
        if (isPlainSequenceTable(tbl, seq_len)) {
            bronze::embed::Local<bronze::Value> arr(scope, bronze::embed::makeArray(seq_len));
            for (uint32_t i = 0; i < seq_len; ++i) {
                bronze::embed::Local<bronze::Value> elem(
                    scope, luaToJs(rt, Value(tbl->array_data[i])));
                arr.set(bronze::embed::setElement(arr.get(), i, elem.get()));
            }
            return arr.get();
        }
        bronze::embed::Local<bronze::Value> obj(scope, bronze::embed::createObject());
        uint64_t key = kNilBits;
        uint64_t val = kNilBits;
        for (;;) {
            key = brasslua_rt_next(v.rawBits(), key, &val);
            if (Value(key).isNil()) break;
            bronze::embed::Local<bronze::Value> js_val(scope, luaToJs(rt, Value(val)));
            Value kv(key);
            if (kv.isString()) {
                std::string kstr(valueToStringView(rt, kv));
                obj.set(bronze::embed::setProperty(obj.get(), kstr, js_val.get()));
            } else if (kv.isInteger()) {
                int64_t idx = kv.asInt64();
                if (idx >= 0 && idx <= static_cast<int64_t>(UINT32_MAX)) {
                    obj.set(bronze::embed::setElement(
                        obj.get(), static_cast<uint32_t>(idx), js_val.get()));
                } else {
                    obj.set(bronze::embed::setProperty(
                        obj.get(), std::to_string(idx), js_val.get()));
                }
            } else {
                std::string kstr = valueToStdString(rt, kv);
                obj.set(bronze::embed::setProperty(obj.get(), kstr, js_val.get()));
            }
        }
        return obj.get();
    }
    // Rule 5: LuaClosure -> JS Function via makeFunction forwarding Lua errors to throwError
    if (v.isFunction()) {
        const auto* cl = static_cast<const LuaClosure*>(v.asPointer());
        uint32_t arity = cl ? cl->arity : 0;
        uint64_t* root_slot = new uint64_t(v.rawBits());
        rt.heap().add_root(root_slot);
        std::shared_ptr<uint64_t> rooted_closure(root_slot, [&rt](uint64_t* p) {
            if (p) {
                rt.heap().remove_root(p);
                delete p;
            }
        });
        return bronze::embed::makeFunction(
            [&rt, rooted_closure](bronze::Value /*thisVal*/,
                                  std::span<const bronze::Value> args) -> bronze::Value {
                std::vector<uint64_t> lua_args;
                lua_args.reserve(args.size());
                for (const bronze::Value& a : args) {
                    lua_args.push_back(jsToLua(rt, a).rawBits());
                }
                try {
                    uint64_t res = brasslua_rt_call(
                        *rooted_closure,
                        lua_args.data(),
                        static_cast<int64_t>(lua_args.size()));
                    return luaToJs(rt, Value(res));
                } catch (const LuaRuntimeError& e) {
                    return bronze::embed::throwError(e.what());
                } catch (const std::exception& e) {
                    return bronze::embed::throwError(e.what());
                }
            },
            arity);
    }
    return bronze::embed::undefined();
}

Value jsToLua(RuntimeState& rt, bronze::Value v, bool convertPlainContainers) {
    // Rule 3: undefined -> nil, null -> nullSentinel
    if (bronze::embed::isUndefined(v)) {
        return Value::nil();
    }
    if (bronze::embed::isNull(v)) {
        return Value::nullSentinel();
    }
    if (bronze::embed::isBool(v)) {
        return Value::fromBool(bronze::embed::toBool(v));
    }
    // Rule 4: BigInt -> Int64; Number -> Int64 if exact safe integer, else Float
    if (bronze::embed::isBigInt(v)) {
        int64_t iv = 0;
        (void)bronze::embed::catchThrow([&]() -> bronze::Value {
            iv = bronze::embed::toInt64(v);
            return bronze::embed::undefined();
        });
        return Value::fromInt64(iv);
    }
    if (bronze::embed::isNumber(v)) {
        double d = bronze::embed::toDouble(v);
        if (std::isfinite(d) && !(d == 0.0 && std::signbit(d)) &&
            d >= static_cast<double>(kMinSafeJsInt) &&
            d <= static_cast<double>(kMaxSafeJsInt)) {
            int64_t iv = static_cast<int64_t>(d);
            if (static_cast<double>(iv) == d) {
                return Value::fromInt64(iv);
            }
        }
        return Value::fromDouble(d);
    }
    // Rule 1: JS String -> LuaString; Uint8Array / ArrayBuffer -> 8-bit clean LuaString
    if (bronze::embed::isString(v)) {
        std::string s = bronze::embed::toUtf8(v);
        return Value(rt.makeStringValue(s));
    }
    if (bronze::embed::isTypedArray(v)) {
        auto info = bronze::embed::typedArrayInfo(v);
        if (info && info.elementKind == bronze::embed::elements::Uint8) {
            const char* chars = reinterpret_cast<const char*>(info.data);
            return Value(rt.makeStringValue(std::string_view(chars, info.byteLength)));
        }
    }
    if (bronze::embed::isArrayBuffer(v)) {
        auto info = bronze::embed::arrayBufferInfo(v);
        const char* chars = info.data ? reinterpret_cast<const char*>(info.data) : "";
        return Value(rt.makeStringValue(std::string_view(chars, info.data ? info.byteLength : 0)));
    }
    // Rule 2: Optional deep container conversion for JS Array / plain JS Object
    if (convertPlainContainers && bronze::embed::isObject(v) &&
        !bronze::embed::isFunction(v) && !bronze::embed::isPromise(v) &&
        !bronze::embed::isTypedArray(v)) {
        bronze::embed::HandleScope scope;
        bronze::embed::Local<bronze::Value> obj(scope, v);
        if (isJsArray(obj.get())) {
            double len_d = bronze::embed::toDouble(bronze::embed::getProperty(obj.get(), "length"));
            uint32_t len = (len_d > 0.0 && len_d <= 67108864.0) ? static_cast<uint32_t>(len_d) : 0;
            LuaTable* tbl = rt.allocTable(len > 0 ? len : 4, 0);
            uint64_t tbl_bits = Value::fromTable(tbl).rawBits();
            for (uint32_t i = 0; i < len; ++i) {
                bronze::Value elem = bronze::embed::getElement(obj.get(), i);
                Value lua_elem = jsToLua(rt, elem, true);
                brasslua_rt_table_set_int(tbl_bits, static_cast<int64_t>(i + 1), lua_elem.rawBits());
            }
            return Value::fromTable(tbl);
        }
        auto obj_glob = bronze::embed::globalValue("Object");
        if (obj_glob.found && bronze::embed::isObject(obj_glob.value)) {
            bronze::embed::Local<bronze::Value> obj_ctor(scope, obj_glob.value);
            bronze::embed::Local<bronze::Value> keys_fn(
                scope, bronze::embed::getProperty(obj_ctor.get(), "keys"));
            if (bronze::embed::isFunction(keys_fn.get())) {
                bronze::Value arg = obj.get();
                auto kr = bronze::embed::call(
                    keys_fn.get(), obj_ctor.get(), std::span<const bronze::Value>(&arg, 1));
                if (!kr.thrown && bronze::embed::isObject(kr.value)) {
                    bronze::embed::Local<bronze::Value> keys_arr(scope, kr.value);
                    uint32_t klen = static_cast<uint32_t>(
                        bronze::embed::toDouble(bronze::embed::getProperty(keys_arr.get(), "length")));
                    LuaTable* tbl = rt.allocTable(0, klen > 0 ? klen : 4);
                    for (uint32_t i = 0; i < klen; ++i) {
                        std::string k = bronze::embed::toUtf8(
                            bronze::embed::getElement(keys_arr.get(), i));
                        bronze::Value prop = bronze::embed::getProperty(obj.get(), k);
                        setTableFieldStr(rt, tbl, k, jsToLua(rt, prop, true));
                    }
                    return Value::fromTable(tbl);
                }
            }
        }
    }
    return wrapBronzeHostObject(rt, v);
}

void attachBronzeBridge(RuntimeState& rt) {
    bronze::embed::bindThreadHeap();
    brass::gc::Heap::set_current(&rt.heap());

    // Register `null`, `await`, and `totable` in Lua `_ENV`
    rt.setGlobal("null", Value::nullSentinel());
    rt.setGlobal("await", makeNativeClosure(rt, &luaAwaitPromise, 1));
    rt.setGlobal("totable", makeNativeClosure(rt, &luaToTableHelper, 1));

    HostBridge bridge;
    bridge.isHostObject = [](uint64_t val) -> bool {
        return isBronzeHostObject(Value(val));
    };

    bridge.getGlobal = [&rt](std::string_view name) -> uint64_t {
        if (name == "globalThis" || name == "window") {
            auto gv = bronze::embed::globalValue(name);
            if (gv.found) {
                return jsToLua(rt, gv.value).rawBits();
            }
            auto gt = bronze::embed::globalValue("globalThis");
            if (gt.found) {
                return jsToLua(rt, gt.value).rawBits();
            }
        }
        auto gv = bronze::embed::globalValue(name);
        if (!gv.found) {
            return kNilBits;
        }
        return jsToLua(rt, gv.value).rawBits();
    };

    bridge.getProperty = [&rt](uint64_t obj, std::string_view key) -> uint64_t {
        bronze::embed::HandleScope scope;
        bronze::embed::Local<bronze::Value> js_obj(scope, unwrapBronzeHostObject(Value(obj)));
        bronze::Value prop = bronze::embed::getProperty(js_obj.get(), key);
        return jsToLua(rt, prop).rawBits();
    };

    bridge.setProperty = [&rt](uint64_t obj, std::string_view key, uint64_t val) {
        bronze::embed::HandleScope scope;
        bronze::embed::Local<bronze::Value> js_obj(scope, unwrapBronzeHostObject(Value(obj)));
        bronze::embed::Local<bronze::Value> js_val(scope, luaToJs(rt, Value(val)));
        (void)bronze::embed::setProperty(js_obj.get(), key, js_val.get());
    };

    bridge.getIndex = [&rt](uint64_t obj, uint64_t key) -> uint64_t {
        bronze::embed::HandleScope scope;
        bronze::embed::Local<bronze::Value> js_obj(scope, unwrapBronzeHostObject(Value(obj)));
        Value kv(key);
        if (kv.isInteger()) {
            int64_t idx = kv.asInt64();
            if (idx >= 0 && idx <= static_cast<int64_t>(UINT32_MAX)) {
                bronze::Value elem = bronze::embed::getElement(js_obj.get(), static_cast<uint32_t>(idx));
                return jsToLua(rt, elem).rawBits();
            }
        }
        std::string kstr = valueToStdString(rt, kv);
        bronze::Value prop = bronze::embed::getProperty(js_obj.get(), kstr);
        return jsToLua(rt, prop).rawBits();
    };

    bridge.setIndex = [&rt](uint64_t obj, uint64_t key, uint64_t val) {
        bronze::embed::HandleScope scope;
        bronze::embed::Local<bronze::Value> js_obj(scope, unwrapBronzeHostObject(Value(obj)));
        bronze::embed::Local<bronze::Value> js_val(scope, luaToJs(rt, Value(val)));
        Value kv(key);
        if (kv.isInteger()) {
            int64_t idx = kv.asInt64();
            if (idx >= 0 && idx <= static_cast<int64_t>(UINT32_MAX)) {
                (void)bronze::embed::setElement(js_obj.get(), static_cast<uint32_t>(idx), js_val.get());
                return;
            }
        }
        std::string kstr = valueToStdString(rt, kv);
        (void)bronze::embed::setProperty(js_obj.get(), kstr, js_val.get());
    };

    bridge.callHost = [&rt](uint64_t callee, uint64_t this_val,
                            const uint64_t* args, size_t nargs) -> uint64_t {
        bronze::embed::HandleScope scope;
        bronze::embed::Local<bronze::Value> fn(scope, unwrapBronzeHostObject(Value(callee)));
        bronze::embed::Local<bronze::Value> recv(
            scope,
            Value(this_val).isNil() ? bronze::embed::undefined()
                                    : luaToJs(rt, Value(this_val)));
        std::vector<bronze::embed::Local<bronze::Value>> rooted_args;
        rooted_args.reserve(nargs);
        for (size_t i = 0; i < nargs; ++i) {
            rooted_args.emplace_back(scope, luaToJs(rt, Value(args[i])));
        }
        std::vector<bronze::Value> raw_args;
        raw_args.reserve(nargs);
        for (const auto& ra : rooted_args) {
            raw_args.push_back(ra.get());
        }
        auto cr = bronze::embed::call(fn.get(), recv.get(), raw_args);
        if (cr.thrown) {
            throw LuaRuntimeError(extractJsErrorMessage(cr.value));
        }
        uint64_t ret = jsToLua(rt, cr.value).rawBits();
        rt.setMultiRet(&ret, 1);
        return ret;
    };

    bridge.callMethod = [&rt](uint64_t obj, std::string_view method,
                              const uint64_t* args, size_t nargs) -> uint64_t {
        bronze::embed::HandleScope scope;
        bronze::embed::Local<bronze::Value> recv(scope, unwrapBronzeHostObject(Value(obj)));
        bronze::embed::Local<bronze::Value> fn(
            scope, bronze::embed::getProperty(recv.get(), method));
        if (!bronze::embed::isFunction(fn.get())) {
            throw LuaRuntimeError("attempt to call a non-function method '" + std::string(method) + "'");
        }
        std::vector<bronze::embed::Local<bronze::Value>> rooted_args;
        rooted_args.reserve(nargs);
        for (size_t i = 0; i < nargs; ++i) {
            rooted_args.emplace_back(scope, luaToJs(rt, Value(args[i])));
        }
        std::vector<bronze::Value> raw_args;
        raw_args.reserve(nargs);
        for (const auto& ra : rooted_args) {
            raw_args.push_back(ra.get());
        }
        auto cr = bronze::embed::call(fn.get(), recv.get(), raw_args);
        if (cr.thrown) {
            throw LuaRuntimeError(extractJsErrorMessage(cr.value));
        }
        uint64_t ret = jsToLua(rt, cr.value).rawBits();
        rt.setMultiRet(&ret, 1);
        return ret;
    };

    bridge.toString = [](uint64_t val) -> std::string {
        Value v(val);
        if (v.isNullSentinel()) {
            return "null";
        }
        if (isBronzeHostObject(v)) {
            return bronze::embed::toUtf8(unwrapBronzeHostObject(v));
        }
        return {};
    };

    rt.setHostBridge(std::move(bridge));
}

ExecutionResult evalLuaInBronzeRealm(std::string_view source, const ExecutionOptions& opts) {
    auto& rt = RuntimeState::current();
    attachBronzeBridge(rt);
    ExecutionResult res = evalLuaSource(source, opts);
    if (bronze::embed::microtasksPending()) {
        bronze::embed::drainMicrotasks();
    }
    return res;
}

ExecutionResult evalLuaFileInBronzeRealm(const std::string& path,
                                         const std::vector<std::string>& script_args,
                                         const ExecutionOptions& opts) {
    auto& rt = RuntimeState::current();
    attachBronzeBridge(rt);
    ExecutionResult res = evalLuaFile(path, script_args, opts);
    if (bronze::embed::microtasksPending()) {
        bronze::embed::drainMicrotasks();
    }
    return res;
}

} // namespace brasslua::bronze_bridge
