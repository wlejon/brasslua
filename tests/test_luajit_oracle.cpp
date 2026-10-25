#include "brasslua/runtime.h"

#include <cstdio>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

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
    std::string tmp_file = "/tmp/brasslua_oracle_case.lua";
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
            "/usr/bin/lua5.4",
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
            "/usr/bin/lua5.4",
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
            "/usr/bin/lua5.4",
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
            "/usr/bin/lua5.4",
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
            "/usr/bin/luajit",
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
            "/usr/bin/luajit",
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
    };

    for (const auto& tc : cases) {
        if (!checkOracle(tc.name, tc.oracle_bin, tc.code)) {
            ++failures;
        }
    }

    return failures;
}

} // namespace brasslua::tests
