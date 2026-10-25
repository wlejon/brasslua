#include "brasslua/runtime.h"

#include <brass/gc/heap.hpp>

#include <cstdio>
#include <iostream>
#include <limits>
#include <string>

namespace brasslua::tests {

namespace {

#define EXPECT_TRUE(cond, msg)                                                      \
    do {                                                                            \
        if (!(cond)) {                                                              \
            std::cerr << "[FAIL] " << __FILE__ << ":" << __LINE__ << ": " << (msg)  \
                      << "\n";                                                      \
            ++failures;                                                             \
        }                                                                           \
    } while (0)

std::string runAndCapture(std::string_view src, bool& ok_out) {
    auto& rt = RuntimeState::current();
    rt.reset();
    std::string out;
    rt.setOutputCallback([&](std::string_view s) { out.append(s); });
    ExecutionResult res = evalLuaSource(src);
    rt.setOutputCallback(nullptr);
    ok_out = res.success;
    if (!res.success) {
        std::cerr << "[ERR] " << res.error_message << "\n";
    }
    return out;
}

} // namespace

int runRuntimeJitTests() {
    int failures = 0;

    // 1. True 64-bit integer subtype vs float subtype (Rule 2)
    {
        bool ok = false;
        std::string out = runAndCapture(R"(
            print(math.type(0), math.type(0.0))
            print(math.type(42), math.type(42.5))
            print(math.type(math.maxinteger), math.type(math.mininteger))
            print(math.maxinteger, math.mininteger)
            print(math.maxinteger + 1 == math.mininteger)
            print(math.mininteger - 1 == math.maxinteger)
            local big = (1 << 62) + 123456789
            print(math.type(big), big)
        )", ok);
        EXPECT_TRUE(ok, "64-bit integer subtype script succeeded");
        std::string expected =
            "integer\tfloat\n"
            "integer\tfloat\n"
            "integer\tinteger\n"
            "9223372036854775807\t-9223372036854775808\n"
            "true\n"
            "true\n"
            "integer\t4611686018550844693\n";
        EXPECT_TRUE(out == expected, "64-bit integer subtype & overflow wrap-around match Lua 5.4");
    }

    // 2. Shared brass::gc::Heap allocation & weak tables (Rule 3)
    {
        bool ok = false;
        std::string out = runAndCapture(R"(
            local wt = setmetatable({}, { __mode = "v" })
            do
                local keep = { name = "alive" }
                wt[1] = keep
                wt[2] = { name = "ephemeral" }
                local dropped = { name = "dropped_local" }
                wt[3] = dropped
                dropped = nil
                do
                    local scoped = { name = "scoped_local" }
                    wt[4] = scoped
                end
                wt[5] = "strings_are_not_weak_collected"
                wt[6] = 12345
                collectgarbage()
                print(wt[1] == keep and wt[2] == nil and wt[3] == nil and wt[4] == nil)
                print(wt[5], wt[6])
            end
            collectgarbage("collect")
            print(wt[1] == nil)
            print(collectgarbage("isrunning"))
        )", ok);
        EXPECT_TRUE(ok && out == "true\nstrings_are_not_weak_collected\t12345\ntrue\ntrue\n",
                    "brass::gc::Heap allocation and collectgarbage work");
    }

    // 3. Stackful Coroutines: create, resume, yield across nested calls, wrap (Rule 2)
    {
        bool ok = false;
        std::string out = runAndCapture(R"(
            local function helper(x)
                local r = coroutine.yield(x * 10, x * 20)
                return r + 5
            end
            local co = coroutine.create(function(a)
                print("status-in:", coroutine.status(co_ref))
                local b = helper(a)
                return b * 2
            end)
            co_ref = co
            print("status-before:", coroutine.status(co))
            local ok1, v1, v2 = coroutine.resume(co, 3)
            print("yield1:", ok1, v1, v2, coroutine.status(co))
            local ok2, final_v = coroutine.resume(co, 100)
            print("final:", ok2, final_v, coroutine.status(co))

            local gen = coroutine.wrap(function()
                for i = 1, 3 do
                    coroutine.yield(i * i)
                end
            end)
            print("wrap:", gen(), gen(), gen())
        )", ok);
        std::string expected =
            "status-before:\tsuspended\n"
            "status-in:\trunning\n"
            "yield1:\ttrue\t30\t60\tsuspended\n"
            "final:\ttrue\t210\tdead\n"
            "wrap:\t1\t4\t9\n";
        EXPECT_TRUE(ok && out == expected, "stackful coroutine.create/resume/yield/wrap work");
    }

    // 4. Static Method Call (`obj:m(x)`) vs Plain Call (`obj.m(x)`) on HostBridge (Rule 6)
    {
        auto& rt = RuntimeState::current();
        rt.reset();
        std::string trace;
        uint64_t fake_host_obj = (static_cast<uint64_t>(Tag::LightUserdata) << kTagShift) | 0xCAFEBABEULL;
        uint64_t fake_host_fn  = (static_cast<uint64_t>(Tag::LightUserdata) << kTagShift) | 0xDEADBEEFULL;

        HostBridge bridge;
        bridge.isHostObject = [&](uint64_t v) {
            return v == fake_host_obj || v == fake_host_fn;
        };
        bridge.getGlobal = [&](std::string_view name) -> uint64_t {
            if (name == "hostObj") return fake_host_obj;
            return kNilBits;
        };
        bridge.getProperty = [&](uint64_t obj, std::string_view key) -> uint64_t {
            if (obj == fake_host_obj && key == "action") return fake_host_fn;
            return kNilBits;
        };
        bridge.callMethod = [&](uint64_t obj, std::string_view method, const uint64_t* args, size_t nargs) -> uint64_t {
            trace += "method:" + std::string(method) +
                     ":this=" + (obj == fake_host_obj ? "hostObj" : "other") +
                     ":nargs=" + std::to_string(nargs) +
                     ":a0=" + std::to_string(Value(args[0]).asInt64()) + ";";
            return Value::fromInt64(111).rawBits();
        };
        bridge.callHost = [&](uint64_t callee, uint64_t this_val, const uint64_t* args, size_t nargs) -> uint64_t {
            (void)callee;
            trace += std::string("plain:this=") + (Value(this_val).isNil() ? "nil" : "set") +
                     ":nargs=" + std::to_string(nargs) +
                     ":a0=" + std::to_string(Value(args[0]).asInt64()) + ";";
            return Value::fromInt64(222).rawBits();
        };
        rt.setHostBridge(bridge);

        auto res = evalLuaSource(R"(
            local r1 = hostObj:action(42)
            local r2 = hostObj.action(99)
            assert(r1 == 111 and r2 == 222)
        )");
        rt.setHostBridge({});
        EXPECT_TRUE(res.success, "hostObj:action(42) and hostObj.action(99) succeeded");
        EXPECT_TRUE(trace == "method:action:this=hostObj:nargs=1:a0=42;plain:this=nil:nargs=1:a0=99;",
                    "compiler statically distinguishes obj:m(x) from obj.m(x)");
    }

    // 5. Lua 5.4 <close> variable attribute (__close metamethod)
    {
        bool ok = false;
        std::string out = runAndCapture(R"(
            local log = {}
            local function make_closer(name)
                return setmetatable({}, {
                    __close = function(self, err)
                        table.insert(log, "closed:" .. name)
                    end
                })
            end
            do
                local a <close> = make_closer("A")
                local b <close> = make_closer("B")
                table.insert(log, "inside")
            end
            print(table.concat(log, ","))
        )", ok);
        EXPECT_TRUE(ok && out == "inside,closed:B,closed:A\n", "Lua 5.4 <close> locals close in reverse order");
    }

    // 6. AOT Object File Compilation
    {
        std::string obj_path = "/tmp/brasslua_aot_test.o";
        bool aot_ok = compileLuaToObjectFile("local x = 0 for i = 1, 100 do x = x + i end return x", obj_path);
        EXPECT_TRUE(aot_ok, "compileLuaToObjectFile emits native .o file");
        std::remove(obj_path.c_str());
    }

    // 7. Shadowed names in closures, per-iteration loop captures, and lexical scoping
    {
        bool ok = false;
        std::string out = runAndCapture(R"(
            local x = 100
            local function shadow_local()
                local x = 200
                x = x + 5
                return x
            end
            local function shadow_param(x)
                x = x * 2
                return x
            end
            local function nested_shadow()
                local x = "mid"
                local inner1 = function()
                    local x = 7
                    return x + 1
                end
                local inner2 = function()
                    return x
                end
                return inner1(), inner2()
            end
            print(shadow_local(), x)
            print(shadow_param(9), x)
            print(x, nested_shadow())

            local funcs = {}
            for i = 1, 4 do
                funcs[i] = function() return i * 10 end
            end
            print(funcs[1](), funcs[2](), funcs[3](), funcs[4]())

            local x = x + 50
            print(x)
        )", ok);
        std::string expected =
            "205\t100\n"
            "18\t100\n"
            "100\t8\tmid\n"
            "10\t20\t30\t40\n"
            "150\n";
        EXPECT_TRUE(ok && out == expected, "shadowed names in closures and loop captures work");
    }

    // 8. Multi-return expansion in `return a, f()`, `obj:m(..., f())`, varargs `...`, and nested >4-arg calls
    {
        bool ok = false;
        std::string out = runAndCapture(R"(
            local function multi3()
                return 10, 20, 30
            end
            local function empty()
            end
            local function prepend_ret()
                return 1, 2, multi3()
            end
            local function prepend_empty()
                return 1, empty()
            end
            print(prepend_ret())
            print(select("#", prepend_empty()), prepend_empty())
            print(select("#", empty()))

            local obj = { base = 100 }
            function obj:sum_all(a, b, c, d, e)
                return self.base + a + b + c + d + (e or 0)
            end
            print(obj:sum_all(1, 2, multi3()))

            local function vararg_fn(a, b, ...)
                local t = { a, ... }
                return b, select("#", ...), table.concat(t, ","), ...
            end
            print(vararg_fn("A", "B", "C", "D", "E"))

            local function sum6(a, b, c, d, e, f)
                return a * 100000 + b * 10000 + c * 1000 + d * 100 + e * 10 + f
            end
            print(sum6(1, 2, 3, 4, sum6(0, 0, 0, 0, 0, 5), 6))
        )", ok);
        std::string expected =
            "1\t2\t10\t20\t30\n"
            "1\t1\n"
            "0\n"
            "163\n"
            "B\t3\tA,C,D,E\tC\tD\tE\n"
            "123456\n";
        EXPECT_TRUE(ok && out == expected,
                    "return a, f(), obj:m(..., f()), varargs, and nested >4-arg calls work");
    }

    {
        bool ok = false;
        std::string out = runAndCapture(R"(
            local M = {}
            M.__index = M
            function M:get() return self.v end
            local o = setmetatable({ v = 99, w = 1 }, M)
            print(o.v, o.w, o:get())
            local p = setmetatable({}, { __index = { v = 7 } })
            print(p.v)
            p.v = 8
            print(p.v)
        )", ok);
        EXPECT_TRUE(ok && out == "99\t1\t99\n7\n8\n",
                    "a table with a table __index reads its own hash-part fields before __index");
    }

    return failures;
}

} // namespace brasslua::tests
