#include "brasslua/bronze_bridge.h"
#include "runtime/runtime_internal.h"
#include "no_error_dialogs.h"

#include <brass/gc/heap.hpp>

#include <cassert>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <span>
#include <string>

using namespace brasslua;
using namespace brasslua::bronze_bridge;

namespace {

void testSharedHeapAndGlobals() {
    auto& rt = RuntimeState::current();
    rt.reset();
    attachBronzeBridge(rt);

    // 1. Verify One Heap Rule: brasslua and bronze share the exact same brass::gc::Heap
    assert(&rt.heap() == brass::gc::Heap::current());

    // 2. Builtin JS globals (Math, JSON) directly accessible from Lua
    auto res = evalLuaInBronzeRealm(R"(
        local m = Math:max(10, 42, 19)
        local j = JSON:parse('{"hello":"world","nums":[10,20,30]}')
        local tbl = totable(j.nums)
        return m + tbl[1] + tbl[2] + tbl[3], j.hello
    )");
    assert(res.success);
    assert(res.return_values.size() == 2);
    assert(res.return_values[0].isInteger() && res.return_values[0].asInt64() == 42 + 60);
    assert(res.return_values[1].isString());

    // 3. Trigger both bronze and Lua GC cycles while live objects exist on both sides
    bronze::embed::collectGarbage();
    auto gc_res = evalLuaInBronzeRealm(R"(
        local keep = { a = "alive_after_gc", b = {1, 2, 3, 4} }
        local js_obj = JSON:parse('{"val": 999}')
        collectgarbage("collect")
        return keep.a, js_obj.val + keep.b[4]
    )");
    assert(gc_res.success);
    assert(gc_res.return_values.size() == 2);
    assert(gc_res.return_values[1].asInt64() == 1003);
    std::printf("  [OK] Shared brass::gc::Heap & JS globals (Math, JSON)\n");
}

void testStringsAndBinaryRule1() {
    auto& rt = RuntimeState::current();
    rt.reset();
    attachBronzeBridge(rt);

    // Register a host function that inspects whether it received a String or Uint8Array
    // and echoes back a mutated Uint8Array for binary inputs.
    bronze::embed::registerGlobal(
        "inspectStringOrBytes",
        bronze::embed::makeFunction(
            [](bronze::Value, std::span<const bronze::Value> args) -> bronze::Value {
                assert(!args.empty());
                if (bronze::embed::isString(args[0])) {
                    std::string s = bronze::embed::toUtf8(args[0]);
                    return bronze::embed::fromUtf8("str:" + s);
                }
                if (bronze::embed::isTypedArray(args[0])) {
                    auto info = bronze::embed::typedArrayInfo(args[0]);
                    assert(info && info.elementKind == bronze::embed::elements::Uint8);
                    size_t len = info.byteLength;
                    const uint8_t* p = info.data;
                    assert(len == 4);
                    assert(p[0] == 0xFF && p[1] == 0x00 && p[2] == 0x80 && p[3] == 0x41);
                    uint8_t out[4] = {p[3], p[2], p[1], p[0]};
                    bronze::embed::HandleScope scope;
                    bronze::embed::Local<bronze::Value> ta(
                        scope, bronze::embed::createTypedArray(bronze::embed::elements::Uint8, 4));
                    bronze::embed::fillTypedArray(ta.get(), std::span<const uint8_t>(out, 4));
                    return ta.get();
                }
                return bronze::embed::undefined();
            },
            1));

    auto res = evalLuaInBronzeRealm(R"(
        local s1 = inspectStringOrBytes("hello UTF-8")
        local bin = string.char(0xFF, 0x00, 0x80, 0x41)
        local rev = inspectStringOrBytes(bin)
        local b1, b2, b3, b4 = string.byte(rev, 1, 4)
        return s1, #rev, b1, b2, b3, b4
    )");
    assert(res.success);
    assert(res.return_values.size() == 6);
    assert(res.return_values[1].asInt64() == 4);
    assert(res.return_values[2].asInt64() == 0x41);
    assert(res.return_values[3].asInt64() == 0x80);
    assert(res.return_values[4].asInt64() == 0x00);
    assert(res.return_values[5].asInt64() == 0xFF);
    std::printf("  [OK] Boundary Rule 1: Strings & 8-bit clean binary <-> Uint8Array\n");
}

void testTablesRule2() {
    auto& rt = RuntimeState::current();
    rt.reset();
    attachBronzeBridge(rt);

    bronze::embed::registerGlobal(
        "verifySeqAndRecord",
        bronze::embed::makeFunction(
            [](bronze::Value, std::span<const bronze::Value> args) -> bronze::Value {
                assert(args.size() == 2);
                bronze::embed::HandleScope scope;
                bronze::embed::Local<bronze::Value> seq(scope, args[0]);
                bronze::embed::Local<bronze::Value> rec(scope, args[1]);

                // seq is a 0-based JS Array [100, 200, 300]
                assert(bronze::embed::toDouble(bronze::embed::getProperty(seq.get(), "length")) == 3.0);
                assert(bronze::embed::toDouble(bronze::embed::getElement(seq.get(), 0)) == 100.0);
                assert(bronze::embed::toDouble(bronze::embed::getElement(seq.get(), 1)) == 200.0);
                assert(bronze::embed::toDouble(bronze::embed::getElement(seq.get(), 2)) == 300.0);

                // rec is a plain JS Object { title: "bro", count: 7 }
                assert(bronze::embed::toUtf8(bronze::embed::getProperty(rec.get(), "title")) == "bro");
                assert(bronze::embed::toDouble(bronze::embed::getProperty(rec.get(), "count")) == 7.0);

                // Return a new 0-based JS Array [5, 6, 7, 8]
                bronze::embed::Local<bronze::Value> out(scope, bronze::embed::makeArray(4));
                for (uint32_t i = 0; i < 4; ++i) {
                    out.set(bronze::embed::setElement(
                        out.get(), i, bronze::embed::fromDouble(static_cast<double>(i + 5))));
                }
                return out.get();
            },
            2));

    auto res = evalLuaInBronzeRealm(R"(
        local js_arr = verifySeqAndRecord({100, 200, 300}, {title = "bro", count = 7})
        local lua_seq = totable(js_arr)
        return #js_arr, #lua_seq, lua_seq[1], lua_seq[4]
    )");
    assert(res.success);
    assert(res.return_values.size() == 4);
    assert(res.return_values[0].asInt64() == 4);
    assert(res.return_values[1].asInt64() == 4);
    assert(res.return_values[2].asInt64() == 5);
    assert(res.return_values[3].asInt64() == 8);
    std::printf("  [OK] Boundary Rule 2: 1-based Lua sequence/record tables <-> JS Array/Object\n");
}

void testNilUndefinedNullRule3() {
    auto& rt = RuntimeState::current();
    rt.reset();
    attachBronzeBridge(rt);

    bronze::embed::registerGlobal(
        "checkNullAndUndefined",
        bronze::embed::makeFunction(
            [](bronze::Value, std::span<const bronze::Value> args) -> bronze::Value {
                assert(args.size() == 2);
                assert(bronze::embed::isNull(args[0]));
                assert(bronze::embed::isUndefined(args[1]));
                return bronze::embed::null();
            },
            2));

    auto res = evalLuaInBronzeRealm(R"(
        local n = checkNullAndUndefined(null, nil)
        return n == null, n == nil, tostring(n)
    )");
    assert(res.success);
    assert(res.return_values.size() == 3);
    assert(res.return_values[0].isBool() && res.return_values[0].asBool() == true);
    assert(res.return_values[1].isBool() && res.return_values[1].asBool() == false);
    assert(valueToStdString(rt, res.return_values[2]) == "null");
    std::printf("  [OK] Boundary Rule 3: nil <-> undefined, null sentinel <-> JS null\n");
}

void testInt64AndBigIntRule4() {
    auto& rt = RuntimeState::current();
    rt.reset();
    attachBronzeBridge(rt);

    bronze::embed::registerGlobal(
        "checkIntAndBigInt",
        bronze::embed::makeFunction(
            [](bronze::Value, std::span<const bronze::Value> args) -> bronze::Value {
                assert(args.size() == 2);
                // arg[0] is safe integer 123456 -> JS Number
                assert(bronze::embed::isNumber(args[0]));
                assert(bronze::embed::toDouble(args[0]) == 123456.0);
                // arg[1] is (1 << 60) + 77 -> JS BigInt
                assert(bronze::embed::isBigInt(args[1]));
                int64_t big = bronze::embed::toInt64(args[1]);
                assert(big == ((1LL << 60) + 77LL));
                // Return big + 1 as BigInt
                int64_t out_big = big + 1;
                bronze::embed::HandleScope scope;
                bronze::embed::Local<bronze::Value> ta(
                    scope, bronze::embed::createTypedArray(bronze::embed::elements::BigInt64, 1));
                bronze::embed::fillTypedArray(
                    ta.get(),
                    std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(&out_big), sizeof(int64_t)));
                return bronze::embed::getElement(ta.get(), 0);
            },
            2));

    auto res = evalLuaInBronzeRealm(R"(
        local big_in = (1 << 60) + 77
        local big_out = checkIntAndBigInt(123456, big_in)
        return math.type(big_out), big_out == (1 << 60) + 78
    )");
    assert(res.success);
    assert(res.return_values.size() == 2);
    assert(valueToStdString(rt, res.return_values[0]) == "integer");
    assert(res.return_values[1].isBool() && res.return_values[1].asBool() == true);
    std::printf("  [OK] Boundary Rule 4: 64-bit Integers <-> JS Number / BigInt\n");
}

void testErrorsAndExceptionsRule5() {
    auto& rt = RuntimeState::current();
    rt.reset();
    attachBronzeBridge(rt);

    bronze::embed::registerGlobal(
        "jsThrower",
        bronze::embed::makeFunction(
            [](bronze::Value, std::span<const bronze::Value>) -> bronze::Value {
                return bronze::embed::throwError("host_js_exception_42");
            },
            0));

    bronze::embed::registerGlobal(
        "callLuaCallbackAndCatch",
        bronze::embed::makeFunction(
            [](bronze::Value, std::span<const bronze::Value> args) -> bronze::Value {
                assert(!args.empty() && bronze::embed::isFunction(args[0]));
                auto cr = bronze::embed::call(args[0], bronze::embed::undefined(), {});
                assert(cr.thrown);
                return bronze::embed::getProperty(cr.value, "message");
            },
            1));

    auto res = evalLuaInBronzeRealm(R"(
        local ok, err = pcall(jsThrower)
        local caught_in_js = callLuaCallbackAndCatch(function()
            error("lua_callback_failed_99")
        end)
        return ok, string.find(err, "host_js_exception_42") ~= nil,
               string.find(caught_in_js, "lua_callback_failed_99") ~= nil
    )");
    assert(res.success);
    assert(res.return_values.size() == 3);
    assert(res.return_values[0].isBool() && res.return_values[0].asBool() == false);
    assert(res.return_values[1].isBool() && res.return_values[1].asBool() == true);
    assert(res.return_values[2].isBool() && res.return_values[2].asBool() == true);
    std::printf("  [OK] Boundary Rule 5: Two-way JS <-> Lua error propagation across pcall & try/catch\n");
}

void testPromisesAndCoroutinesRule6() {
    auto& rt = RuntimeState::current();
    rt.reset();
    attachBronzeBridge(rt);

    bronze::embed::Persistent saved_resolver;
    bronze::embed::registerGlobal(
        "makeDeferredPromise",
        bronze::embed::makeFunction(
            [&saved_resolver](bronze::Value, std::span<const bronze::Value>) -> bronze::Value {
                bronze::embed::HandleScope scope;
                auto p_glob = bronze::embed::globalValue("Promise");
                assert(p_glob.found);
                bronze::embed::Local<bronze::Value> p_ctor(scope, p_glob.value);
                bronze::embed::Local<bronze::Value> executor(
                    scope,
                    bronze::embed::makeFunction(
                        [&saved_resolver](bronze::Value,
                                          std::span<const bronze::Value> ex_args) -> bronze::Value {
                            if (!ex_args.empty()) {
                                saved_resolver.set(ex_args[0]);
                            }
                            return bronze::embed::undefined();
                        },
                        2));
                bronze::Value arg = executor.get();
                auto cr = bronze::embed::construct(p_ctor.get(), std::span<const bronze::Value>(&arg, 1));
                assert(!cr.thrown);
                return cr.value;
            },
            0));

    bronze::embed::registerGlobal(
        "resolveDeferred",
        bronze::embed::makeFunction(
            [&saved_resolver](bronze::Value, std::span<const bronze::Value> args) -> bronze::Value {
                bronze::Value val = args.empty() ? bronze::embed::undefined() : args[0];
                (void)bronze::embed::call(
                    saved_resolver.get(), bronze::embed::undefined(),
                    std::span<const bronze::Value>(&val, 1));
                bronze::embed::drainMicrotasks();
                return bronze::embed::undefined();
            },
            1));

    auto res = evalLuaInBronzeRealm(R"(
        -- 1. Immediately resolved & rejected promises
        local v1 = await(Promise:resolve(300))
        local ok_rej, err_rej = pcall(function()
            return await(Promise:reject("rejected_reason"))
        end)

        -- 2. Deferred promise awaited inside a Lua coroutine
        local coro_result = 0
        local co = coroutine.create(function()
            local x = await(makeDeferredPromise())
            coro_result = x + 21
        end)
        local ok_start = coroutine.resume(co)
        local status_before = coroutine.status(co)
        resolveDeferred(400)
        local status_after = coroutine.status(co)

        return v1, ok_rej, string.find(err_rej, "rejected_reason") ~= nil,
               ok_start, status_before, status_after, coro_result
    )");
    assert(res.success);
    assert(res.return_values.size() == 7);
    assert(res.return_values[0].asInt64() == 300);
    assert(res.return_values[1].asBool() == false);
    assert(res.return_values[2].asBool() == true);
    assert(res.return_values[3].asBool() == true);
    assert(valueToStdString(rt, res.return_values[4]) == "suspended");
    assert(valueToStdString(rt, res.return_values[5]) == "dead");
    assert(res.return_values[6].asInt64() == 421);
    std::printf("  [OK] Boundary Rule 6: Promises & Lua coroutine yield/resume via await(promise)\n");
}

void testMethodCallVsPlainCall() {
    auto& rt = RuntimeState::current();
    rt.reset();
    attachBronzeBridge(rt);

    {
        bronze::embed::HandleScope scope;
        bronze::embed::Local<bronze::Value> obj(scope, bronze::embed::createObject());
        obj.set(bronze::embed::setProperty(obj.get(), "base", bronze::embed::fromDouble(50.0)));
        bronze::embed::Local<bronze::Value> add_fn(
            scope,
            bronze::embed::makeFunction(
                [](bronze::Value thisVal, std::span<const bronze::Value> args) -> bronze::Value {
                    if (bronze::embed::isUndefined(thisVal)) {
                        return bronze::embed::fromDouble(-1.0);
                    }
                    double base = bronze::embed::toDouble(bronze::embed::getProperty(thisVal, "base"));
                    double x = args.empty() ? 0.0 : bronze::embed::toDouble(args[0]);
                    return bronze::embed::fromDouble(base + x);
                },
                1));
        obj.set(bronze::embed::setProperty(obj.get(), "compute", add_fn.get()));
        bronze::embed::registerGlobal("hostCounter", obj.get());
    }

    auto res = evalLuaInBronzeRealm(R"(
        -- Method call `obj:compute(15)` passes `obj` as `this` -> 50 + 15 = 65
        local m_res = hostCounter:compute(15)
        -- Plain call `obj.compute(15)` passes `undefined` as `this` -> -1
        local p_res = hostCounter.compute(15)
        return m_res, p_res
    )");
    assert(res.success);
    assert(res.return_values.size() == 2);
    assert(res.return_values[0].asInt64() == 65);
    assert(res.return_values[1].asInt64() == -1);
    std::printf("  [OK] Method call `obj:m(x)` vs plain call `obj.m(x)` receiver semantics\n");
}

} // namespace

int main() {
    brasslua::tests::disableErrorDialogs();
    std::printf("=== Running brasslua_bronze Bridge & Shared Heap Tests ===\n");
    testSharedHeapAndGlobals();
    testStringsAndBinaryRule1();
    testTablesRule2();
    testNilUndefinedNullRule3();
    testInt64AndBigIntRule4();
    testErrorsAndExceptionsRule5();
    testPromisesAndCoroutinesRule6();
    testMethodCallVsPlainCall();
    std::printf("[PASS] All brasslua_bronze bridge tests passed!\n");
    return 0;
}
