-- Lua 5.4's numeric for: integer or float loop by the initial value and
-- step, the limit clipped to the integers, no overflow at the ends, and the
-- errors of forprep. Run against the reference interpreter.
local function f(lo, hi, st)
  local out = {}
  for i = lo, hi, st do out[#out + 1] = tostring(i) end
  return table.concat(out, " ")
end
local function err(...)
  local ok, e = pcall(f, ...)
  return ok and "ok" or (tostring(e):gsub("^.-:%d+: ", ""))
end
local maxi, mini = math.maxinteger, math.mininteger
print(f(1, 3, 1), f(1.0, 3, 1), f(1, 2, 0.5), f(3, 1, -1), f(1, 3.5, 1), f(3, 0.5, -1))
print(f(maxi - 2, maxi, 1), f(mini + 2, mini, -1), f(maxi, maxi, 1), f(mini, mini, -1))
print(f(1, math.huge, 1 << 62), f(5, -math.huge, -(1 << 62)), f(1, -math.huge, 1), f(1, 0/0, 1))
print(f(mini, maxi, maxi), f(maxi, mini, mini), f(mini, maxi, 1 << 62))
print(f(maxi - 5, maxi, 4), f(mini + 5, mini, -4), f("1", 3, 1), f(1, "3", 1))
print(err(1, 2, 0), err(1.0, 2, 0.0), err(1, {}, 1), err(1, 2, "x"), err(nil, 2, 1))
print(err(1, 2, false), err(1, nil, 1))
for i = maxi - 1, maxi do io.write(i, " ") end
for i = mini + 1, mini, -1 do io.write(i, " ") end
for i = maxi - 3, maxi, 2 do io.write(i, " ") end
print()
local n = 4
for i = n - 1, n do io.write(math.type(i), " ") end
for i = 1, 2, 1.0 do io.write(math.type(i), " ") end
for i = 1.5, 2 do io.write(math.type(i), " ") end
print()
local c = 0
for i = 1, n * 1000, 7 do c = c + i end
for i = n * 1000, 1, -7 do c = c - i end
print(c)
