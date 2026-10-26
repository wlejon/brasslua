-- _ENV: free names are fields of the _ENV in scope (a local, a parameter or
-- the chunk's), globals are the fields of _G, and _G written to directly is
-- what a global read sees. Run against the reference interpreter.
print(_ENV == _G, rawequal(_ENV, _G))
x = 1 _G.x = 2 print(x)
rawset(_G, "y", 3) print(y)
local function getx() return x end
_G.x = 5 print(getx())
do
  local _ENV = {print = print, x = "inner"}
  print(x, y)
  z = 7
  function g() return x end
  print(z, g())
end
print(z, g)
local function with(_ENV) return a + b end
print(with({a = 1, b = 2}))
local t = setmetatable({}, {__index = _G})
do
  local _ENV = t
  w = 9
  print(w, rawget(t, "w"), rawget(_G, "w"))
end
local f = load("x = 10 return x", "c", "t")
print(f(), x)
local saved = _ENV
local function setenv(e) _ENV = e end
setenv(setmetatable({}, {__index = saved}))
q = 1
print(q, saved.q, rawget(saved, "q"))
setmetatable(saved, {__index = function(_, k) return k .. "!" end,
                     __newindex = function(tb, k, v) rawset(tb, k, v * 10) end})
saved.print(saved.missing)
