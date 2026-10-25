local function show(...)
  local t = table.pack(...)
  local out = {}
  for i = 1, t.n do out[i] = tostring(t[i]) end
  print(table.concat(out, " | "))
end
local function try(f, ...)
  local r = table.pack(pcall(f, ...))
  if r[1] then show(table.unpack(r, 2, r.n)) else print("ERR", r[2]) end
end

-- numbers from strings
show(tonumber("10"), tonumber("0x10"), tonumber("010"), tonumber("  12  "), tonumber("1e2"))
show(tonumber("0x1p4"), tonumber("inf"), tonumber("nan"), tonumber(""), tonumber("1 2"))
show(tonumber("9223372036854775807"), tonumber("9223372036854775808"), tonumber("-9223372036854775808"))
show(tonumber("ff", 16), tonumber("zz", 36), tonumber("-101", 2), tonumber("8", 8), tonumber(" 7 ", 10))
show("10" + 1, "3" * "4", "2" ^ 2, "7" // 2, "7" % 3, -"3", "0x10" + 0, "1.5" + 1)
show(math.type("10" + 1), math.type("10.0" + 1))
try(tonumber)
try(tonumber, 10, 16)
try(tonumber, "10", 99)

-- sub / byte / char / rep / reverse / lower / upper / len
local s = "hello world"
show(s:sub(1, 5), s:sub(-5), s:sub(0), s:sub(7, 100), s:sub(5, 2), s:sub(-100, 2), ("x"):sub(2))
show(s:byte(), s:byte(1, 3), s:byte(-1), s:byte(100))
show(string.char(72, 105), string.char())
try(string.char, 256)
try(string.char, "x")
show(("ab"):rep(3), ("ab"):rep(3, ","), ("ab"):rep(0), ("ab"):rep(-1), (""):rep(5))
show(s:reverse(), s:upper(), ("MiXeD"):lower(), #s, s:len())
show(string.len(123), string.sub(12345, 2, 3), string.upper(1.5))
try(string.sub)
try(string.sub, "x")
try(string.rep, "x", 1.5)

-- find
show(s:find("o"), s:find("o", 6), s:find("o", -3), s:find("xyz"), s:find(""), s:find("", 20))
show(s:find("o w"), s:find("l+"), s:find("(l+)(o)"), s:find("%s"), s:find(".", 1, true))
show(("a.b"):find(".", 1, true), ("a.b"):find("%."), ("a+b"):find("+", 1, true))
show(s:find("^hello"), s:find("^world"), s:find("world$"), s:find("hello$"))
show(("aaa"):find("a-"), ("aaa"):find("a-$"), ("aaa"):find("a*"), ("aaa"):find("a?a"))
-- match
show(s:match("(%w+) (%w+)"), s:match("()ll()"), s:match("%d"), ("key=val"):match("(%w+)=(%w+)"))
show(("  trim  "):match("^%s*(.-)%s*$"), ("[[x]]"):match("%[(%b[])%]"), ("f(a(b)c)"):match("%b()"))
show(("THE (quick) fox"):find("%f[%a]%a+"), ("THE (quick) fox"):match("%f[%l]%a+"))
show(("hello"):match(".-(l+)(.*)"), ("x=1, y=2"):match("y=(%d)"), ("abc"):match("((a)(b))"))
show(("abcabc"):match("(abc)%1"), ("abab"):match("(ab)%1"), ("2024-01-15"):match("(%d+)-(%d+)-(%d+)"))
show(("[a]"):match("[%]]"), ("a-z"):match("[a%-z]+"), ("^x"):match("[%^x]+"), ("hello"):match("[^aeiou]+"))
show(("x"):match("[]]"), ("]"):match("[]]"), ("a]"):match("[^]]"))
try(string.match, "x", "(")
try(string.match, "x", "%")
try(string.match, "x", "[a")
try(string.match, "x", "(()")
try(string.match, "x", "%1")
try(string.match, "x", "x)")
try(string.match, "x", "%f")
try(string.match, "x", "%b")
-- gmatch
local words = {}
for w in ("one two  three"):gmatch("%a+") do words[#words + 1] = w end
show(table.concat(words, ","))
for k, v in ("a=1, b=2, c=3"):gmatch("(%w+)=(%w+)") do io.write(k, v, ";") end
print()
for p in ("abc"):gmatch("()") do io.write(p, " ") end
print()
for x in ("abc"):gmatch("") do io.write("[", x, "]") end
print()
for x in ("hello world"):gmatch("o", 6) do io.write(x) end
print()
-- gsub
show(s:gsub("o", "0"), s:gsub("o", "0", 1), s:gsub("", "-"), s:gsub("l+", "%0%0"))
show(s:gsub("(%w+)", "<%1>"), s:gsub("(%w+) (%w+)", "%2 %1"), s:gsub("%w+", "%%"))
show(s:gsub("%w+", {hello = "HI", world = false}), s:gsub("%w+", function(w) return #w end))
show(s:gsub("%w+", function(w) if w == "hello" then return nil end return w:upper() end))
show(("abc"):gsub("", "/"), ("abc"):gsub("b*", "-"), ("abc"):gsub("^a", "A"), ("aaa"):gsub("^a", "A"))
show(("hello"):gsub("()", "%1"), ("x"):gsub("x", 5), ("x y"):gsub("(%w)", "%1%1", 1))
try(string.gsub, "x", "x", "%2")
try(string.gsub, "x", "x", "%z")
try(string.gsub, "x", "x", true)
try(string.gsub, "x", "x", function() return {} end)
try(string.gsub, "x", "x")

-- format
show(string.format("%d %5d %-5d| %05d %+d", 42, 42, 42, 42, 42))
show(string.format("%x %X %o %#x", 255, 255, 8, 255))
show(string.format("%5.2f %e %g %g %g", 3.14159, 12345.678, 0.0001, 1e20, 100))
show(string.format("%s %10s %-10s| %.2s", "hi", "hi", "hi", "hello"))
show(string.format("%q", 'a "quoted"\n\\ string\0 with\1 ctrl\0011'))
show(string.format("%q %q %q %q", 1, -5, math.mininteger, 1.5))
show(string.format("%q %q %q", 1/0, -1/0, true), string.format("%q", nil))
show(string.format("%c%c%c", 76, 117, 97), string.format("%%"), string.format("%5s%%", "x"))
show(string.format("%s %s %s", 1, 1.0, true), string.format("%s", setmetatable({}, {__tostring = function() return "OBJ" end})))
show(string.format("%d", "10"), string.format("%.3f", "1.5"), string.format("%i", 3.0))
show(string.format("%-5d|", -3), string.format("%.0f", 0.5), string.format("%.14g", 0.1))
try(string.format, "%d", 1.5)
try(string.format, "%d", "x")
try(string.format, "%d")
try(string.format, "%y", 1)
try(string.format, "%10.123f", 1)
try(string.format, "%123d", 1)
try(string.format, "%q", {})
try(string.format, "%5q", 1)
try(string.format, "%")
try(string.format, "%#d", 1)

-- pack
local p = string.pack("i4", 100)
show(#p, string.unpack("i4", p))
show(string.unpack("<i2", string.pack("<i2", -2)), string.unpack(">I2", string.pack(">I2", 258)))
show(string.packsize("i4i8"), string.packsize("!8i1i8"), string.packsize("bhd"))
show(string.unpack("z", string.pack("z", "hello")), string.unpack("s1", string.pack("s1", "abc")))
show(string.unpack("c3", "abcdef"), string.unpack("c3", "abcdef", 4))
show(string.unpack("d", string.pack("d", 1.5)), string.unpack("f", string.pack("f", 0.5)))
show(string.unpack("i16", string.pack("i16", -3)), string.unpack("<j", string.pack("<j", math.maxinteger)))
try(string.pack, "i17", 1)
try(string.pack, "i1", 200)
try(string.unpack, "i4", "ab")
try(string.packsize, "s")
try(string.pack, "y", 1)

-- table library
local t = {1, 2, 3}
table.insert(t, 4); table.insert(t, 1, 0)
show(table.concat(t, ","), #t)
show(table.remove(t), table.remove(t, 1), table.concat(t, ","))
try(table.insert, t, 10, 5)
try(table.insert, t, 1, 2, 3)
try(table.insert, nil, 1)
show(table.concat({1, 2.5, "x"}, "-"), table.concat({}, ","), table.concat({1, 2, 3}, ",", 2), table.concat({1, 2, 3}, ",", 2, 1))
try(table.concat, {1, {}, 3})
show(table.unpack({1, 2, 3})); show(table.unpack({1, 2, 3}, 2)); show(table.unpack({1, 2, 3}, 2, 5))
show(select("#", table.unpack({}, 1, 0)), select("#", table.unpack({1, nil, 3})))
local pk = table.pack(1, nil, 3)
show(pk.n, pk[1], pk[2], pk[3])
show(table.concat(table.move({1, 2, 3}, 1, 3, 2), ","), table.concat(table.move({1, 2, 3}, 2, 3, 1), ","))
show(table.concat(table.move({1, 2, 3}, 1, 3, 1, {}), ","))
local arr = {5, 3, 8, 1, 9, 2, 7}
table.sort(arr); show(table.concat(arr, ","))
table.sort(arr, function(a, b) return a > b end); show(table.concat(arr, ","))
local names = {"bob", "alice", "Carol", "dave"}
table.sort(names); show(table.concat(names, ","))
try(table.sort, {1, "x", 2})
try(table.sort, {3, 2, 1}, 5)
local big = {}
for i = 1, 300 do big[i] = (i * 7919) % 301 end
table.sort(big)
local ok = true
for i = 2, 300 do if big[i - 1] > big[i] then ok = false end end
show(ok, big[1], big[300])
print(unpack)
