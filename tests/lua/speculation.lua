-- Speculation: the hints bet on what the chunk's own code suggests (a field
-- written with floats holds floats), and a value that is not what was bet
-- on finishes the call in the generic code, mid-expression and mid-loop,
-- errors included. The values that break a bet come from calls the hints
-- cannot see into, so the bets are still made. Run against the reference
-- interpreter.
local function opaque(v) return select(1, v) end
local t = {x = 1.5, n = 2}
local function f(o) return o.x * 2 + o.n end
local function g(o) local s = 0 for i = 1, 4 do s = s + o.x * i end return s end
print(f(t), f({x = 2.5, n = 1}), f({x = opaque("3"), n = 1}), f({x = opaque(2), n = 1}),
      math.type(f({x = opaque(2), n = 1})))
print((pcall(f, {x = opaque({}), n = 1})))
print(g(t), g({x = opaque(3)}), g({x = opaque("0.5")}))
local mt = {__mul = function(a, b) return 10 end}
print(f({x = setmetatable({}, mt), n = 1}))
local sum = 0
local items = {}
for i = 1, 10 do items[i] = {x = i + 0.5, n = 1} end
items[7] = {x = opaque("7"), n = 1}
for i = 1, #items do sum = sum + f(items[i]) end
print(sum)
local function cmp(o) if o.x < 3.5 then return "lt" end return "ge" end
print(cmp(t), cmp({x = 5.5}), cmp({x = opaque(1)}), (pcall(cmp, {x = opaque("a")})))
-- Integers: a bet on two integers that overflows or meets a float.
local function h(o) return o.a + o.b end
local p = {a = 1, b = 2}
print(h(p), h({a = opaque(math.maxinteger), b = 1}), h({a = opaque(0.5), b = 1}), h({a = opaque("4"), b = 1}))
-- Keys bet on as integers, and equality bet on as integers.
local arr = {10, 20, 30}
arr.k = "field"
local function at(i) return arr[i] end
local function put(i, v) arr[i] = v end
print(at(2), at(opaque(3.0)), at(opaque("k")), at(opaque(2.5)))
put(1, 11); put(opaque(2.0), 22); put(opaque("z"), "zz")
print(arr[1], arr[2], arr.z)
local function same(a, b) return a == b, a ~= b end
print(same(1, 1), same(2, 3))
print(same(opaque(1.0), 1), same(opaque("x"), opaque("x")), same(opaque(0 / 0), opaque(0 / 0)))
print(same(opaque(math.maxinteger), opaque(math.maxinteger + 0.0)), same(opaque(2 ^ 53), opaque(2 ^ 53 + 1)))
-- An integer a double does not hold, against a double: compared exactly.
local f53 = 9007199254740992.0
for k = 9007199254740991, 9007199254740993 do print(k == f53, k < f53, k <= f53, k > f53) end
local big = math.maxinteger
print(big == big + 0.0, big < big + 0.0, -big - 1 == -2 ^ 63, -big - 1 < -2 ^ 63)
