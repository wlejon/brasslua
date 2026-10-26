-- Strings made at run time are collected; equal strings made again are
-- equal, as keys and as values, across collections.
local keep = {}
for i = 1, 20000 do
  local s = "k" .. i
  if i % 1000 == 0 then keep[#keep + 1] = s end
end
collectgarbage()
collectgarbage()
local t = {}
for i = 1, 20000 do t["k" .. i] = i end
collectgarbage()
local sum = 0
for i = 1, 20000 do sum = sum + t["k" .. i] end
print(sum, #keep, keep[1] == "k1000", keep[20] == "k" .. 20000)
t = nil
collectgarbage()
local u = {}
for i = 1, 5000 do u[#u + 1] = ("v" .. i):upper() end
collectgarbage()
print(u[1], u[5000], u[2500] == "V2500")
for round = 1, 3 do
  local m = {}
  for i = 1, 3000 do m[string.format("%d:%d", round, i)] = i end
  collectgarbage()
  local c = 0
  for k, v in pairs(m) do if m[k] == v then c = c + 1 end end
  print(round, c, m[round .. ":1500"])
end
local before = collectgarbage("count")
for i = 1, 200000 do local _ = "tmp" .. i end
collectgarbage()
collectgarbage()
print(collectgarbage("count") < before + 4096)
