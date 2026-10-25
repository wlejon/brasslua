-- Long strings (over 40 bytes) as table keys, equality and next.
local base = string.rep("abcdefghij", 6)
local t = {}
local k1 = base .. "1"
local k2 = base .. "1"
print(k1 == k2, rawequal(k1, k2), #k1)
t[k1] = "one"
print(t[k2], t[base .. "1"], t[base .. "2"])
t[base .. "1"] = "uno"
local n = 0
for k, v in pairs(t) do n = n + 1; print(#k, v) end
print(n)
print(next(t, k2))
-- a long constant and an equal string made at run time
local c = "abcdefghijabcdefghijabcdefghijabcdefghijabcdefghijabcdefghij1"
print(c == k1, t[c])
t[c] = "const"
print(t[k1])
local u = {[c] = 5}
print(u[k2])
-- many long keys, collected and remade
local m = {}
for i = 1, 2000 do m[base .. i] = i end
collectgarbage()
local s = 0
for i = 1, 2000 do s = s + m[base .. i] end
print(s)
for i = 1, 2000, 2 do m[base .. i] = nil end
local cnt = 0
for k, v in pairs(m) do cnt = cnt + 1 end
print(cnt)
-- long strings in comparisons and sort
local arr = {}
for i = 1, 50 do arr[i] = base .. string.format("%03d", (i * 37) % 50) end
table.sort(arr)
print(arr[1] == base .. "000", arr[50] == base .. "049")
print((base .. "x") < (base .. "y"), (base .. "x") == (base .. "y"))
-- concat of long pieces
local parts = {}
for i = 1, 100 do parts[i] = base end
local big = table.concat(parts)
print(#big, big == string.rep(base, 100), big:sub(1, 10))
-- rawget/rawset with long keys
local r = {}
rawset(r, base .. "r", 7)
print(rawget(r, base .. "r"), rawget(r, base .. "q"))
