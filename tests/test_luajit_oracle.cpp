#include "brasslua/runtime.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#if defined(_WIN32)
#define popen _popen
#define pclose _pclose
#endif

namespace brasslua::tests {

namespace {

std::string runExternalCommand(const std::string& cmd) {
    std::string out;
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) return {};
    char buf[512];
    while (fgets(buf, sizeof(buf), pipe) != nullptr) {
        out.append(buf);
    }
    pclose(pipe);
    return out;
}

// The oracle interpreters are found by name on PATH, so the suite runs
// wherever lua5.4 and luajit are installed (on Windows, as lua5.4.exe and
// luajit.exe). A missing one is reported and its cases skipped, not passed.
bool oracleAvailable(const char* binary) {
#if defined(_WIN32)
    std::string probe = std::string("where ") + binary + " >NUL 2>NUL";
#else
    std::string probe = std::string("command -v ") + binary + " >/dev/null 2>&1";
#endif
    return std::system(probe.c_str()) == 0;
}

std::string runBrassLua(std::string_view code, bool& ok_out) {
    auto& rt = RuntimeState::current();
    rt.reset();
    std::string out;
    rt.setOutputCallback([&](std::string_view s) { out.append(s); });
    ExecutionResult res = evalLuaSource(code);
    rt.setOutputCallback(nullptr);
    ok_out = res.success;
    if (!res.success) {
        std::cerr << "[BRASSLUA ERR] " << res.error_message << "\n";
    }
    return out;
}

bool checkOracle(const char* name, const char* binary_path, std::string_view lua_code) {
    std::string tmp_file =
        (std::filesystem::temp_directory_path() / "brasslua_oracle_case.lua").string();
    {
        std::ofstream ofs(tmp_file, std::ios::binary);
        ofs.write(lua_code.data(), static_cast<std::streamsize>(lua_code.size()));
    }
    std::string expected = runExternalCommand(std::string(binary_path) + " " + tmp_file);
    std::remove(tmp_file.c_str());

    bool ok = false;
    std::string actual = runBrassLua(lua_code, ok);
    if (!ok || actual != expected) {
        std::cerr << "[FAIL] Oracle mismatch in '" << name << "' against " << binary_path << ":\n"
                  << "  Expected:\n" << expected
                  << "  Actual:\n" << actual << "\n";
        return false;
    }
    return true;
}

} // namespace

int runLuaOracleTests() {
    int failures = 0;

    struct OracleCase {
        const char* name;
        const char* oracle_bin;
        const char* code;
    };

    const std::vector<OracleCase> cases = {
        {
            "lua54_integers_and_bitwise",
            "lua5.4",
            R"(
                print(math.type(123), math.type(123.0))
                print(math.maxinteger, math.mininteger)
                print(math.maxinteger ~ math.mininteger)
                print((1 << 62) >> 31)
                print((-17) // 5, (-17) % 5)
                print(17 // (-5), 17 % (-5))
                print(math.ult(-1, 1), math.ult(1, -1))
            )"
        },
        {
            "lua54_tables_and_metatables",
            "lua5.4",
            R"(
                local Vec = {}
                Vec.__index = Vec
                function Vec.new(x, y)
                    return setmetatable({ x = x, y = y }, Vec)
                end
                function Vec.__add(a, b)
                    return Vec.new(a.x + b.x, a.y + b.y)
                end
                function Vec:len2()
                    return self.x * self.x + self.y * self.y
                end
                local v1 = Vec.new(3, 4)
                local v2 = Vec.new(1, 2)
                local v3 = v1 + v2
                print(v3.x, v3.y, v3:len2())
            )"
        },
        {
            "lua54_closures_and_upvalues",
            "lua5.4",
            R"(
                local function make_counter(start, step)
                    local n = start
                    return function()
                        n = n + step
                        return n
                    end
                end
                local c1 = make_counter(10, 5)
                local c2 = make_counter(100, -10)
                print(c1(), c1(), c2(), c1(), c2())
            )"
        },
        {
            "lua54_coroutines_and_generators",
            "lua5.4",
            R"(
                local function fib_gen(n)
                    return coroutine.wrap(function()
                        local a, b = 0, 1
                        for i = 1, n do
                            coroutine.yield(a)
                            a, b = b, a + b
                        end
                    end)
                end
                local out = {}
                for v in fib_gen(8) do
                    table.insert(out, tostring(v))
                end
                print(table.concat(out, ","))
            )"
        },
        {
            "luajit_sieve_and_algorithms",
            "luajit",
            R"(
                local is_prime = {}
                local N = 200
                for i = 2, N do is_prime[i] = true end
                for i = 2, 14 do
                    if is_prime[i] then
                        for j = i * i, N, i do
                            is_prime[j] = false
                        end
                    end
                end
                local count = 0
                for i = 2, N do
                    if is_prime[i] then count = count + 1 end
                end
                print("primes<=200:", count)
            )"
        },
        {
            "luajit_bitops_and_strings",
            "luajit",
            R"(
                local x = 0x12345678
                for i = 1, 50 do
                    x = bit.bxor(x, bit.lshift(x, 3))
                    x = bit.bor(bit.rshift(x, 2), i)
                end
                print(bit.tohex(x))
                local s = string.format("%s-%04d", ("brasslua"):upper(), 42)
                print(s, #s, string.sub(s, 1, 5))
            )"
        },
        {
            "lua54_shadowing_multiret_and_nested_calls",
            "lua5.4",
            R"(
                local x = 10
                local function f()
                    local x = 20
                    local function g()
                        local x = 30
                        return x
                    end
                    return x, g()
                end
                print(f(), x)

                local function pair_plus(a)
                    return a, a + 1, a + 2
                end
                local function prefix_ret(x, y)
                    return x, y, pair_plus(100)
                end
                print(prefix_ret(1, 2))

                local Box = { offset = 1000 }
                function Box:collect(a, b, c, d, e)
                    return self.offset + a + b + c + d + e
                end
                print(Box:collect(1, 2, pair_plus(10)))

                local function take7(a, b, c, d, e, f, g)
                    return table.concat({a, b, c, d, e, f, g}, "-")
                end
                print(take7(1, 2, 3, 4, take7(5, 6, 7, 8, 9, 10, 11), 12, 13))
            )"
        },
        {
            "lua54_gmatch_captures_hash_and_oop",
            "lua5.4",
            R"LUA(
                local words = {}
                for k, v in string.gmatch("foo=10, bar=20, baz=30", "(%a+)=(%d+)") do
                    words[#words + 1] = k .. ":" .. v
                end
                print(table.concat(words, "|"))

                local s1, e1, c1, c2 = string.find("hello 123 world", "(%a+) (%d+)")
                print(s1, e1, c1, c2)

                local p1, m1, p2 = string.match("  abc  ", "()(%a+)()")
                print(p1, m1, p2)

                local sub_s, sub_n = string.gsub("a=1 b=2", "(%a)=(%d)", "%2->%1")
                print(sub_s, sub_n)

                local bal = string.match("x (a (b) c) y", "%b()")
                print(bal)

                local dict = {}
                for i = 1, 200 do
                    dict["key_" .. i] = i * 3
                end
                local dsum = 0
                for i = 1, 200 do
                    dsum = dsum + dict["key_" .. i]
                end
                print(dsum)
            )LUA"
        },
    };

    for (const auto& tc : cases) {
        if (!oracleAvailable(tc.oracle_bin)) {
            std::cerr << "[SKIP] '" << tc.name << "': oracle " << tc.oracle_bin << " is not on PATH\n";
            continue;
        }
        if (!checkOracle(tc.name, tc.oracle_bin, tc.code)) {
            ++failures;
        }
    }

    return failures;
}

} // namespace brasslua::tests
