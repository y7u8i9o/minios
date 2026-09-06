-- Language and standard library checks of the Lua port on the minios
-- libc. Each section prints PASS; an assertion failure stops the script
-- with a traceback and a non zero exit status.

-- Integer and bitwise operations
assert(1 + 2 == 3)
assert(7 // 2 == 3)
assert(-7 // 2 == -4)
assert(7 % 3 == 1)
assert(-7 % 3 == 2)
assert(1 << 10 == 1024)
assert(0xFF & 0x0F == 0x0F)
assert(0xF0 | 0x0F == 0xFF)
assert(~0 == -1)
assert(math.maxinteger + 1 == math.mininteger)
print("Integer and bitwise: PASS")

-- Float and math library
assert(2 ^ 0.5 == math.sqrt(2))
assert(1 / 0 == math.huge)
assert(-1 / 0 == -math.huge)
assert(0 / 0 ~= 0 / 0)
assert(math.abs(-42) == 42)
assert(math.floor(3.7) == 3)
assert(math.ceil(3.2) == 4)
assert(math.type(1) == "integer")
assert(math.type(1.0) == "float")
assert(math.type("x") == nil)   -- the manual specifies fail (nil) for a non number
assert(math.sin(0) == 0.0)
assert(math.cos(0) == 1.0)
assert(math.exp(0) == 1.0)
assert(math.log(1) == 0.0)
assert(math.log(math.exp(1)) - 1.0 < 1e-14)
print("Float and math library: PASS")

-- String library
assert(string.len("hello") == 5)
assert(string.rep("ab", 3) == "ababab")
assert(string.reverse("abcd") == "dcba")
assert(string.upper("hello") == "HELLO")
assert(string.lower("HELLO") == "hello")
assert(string.byte("A") == 65)
assert(string.char(65) == "A")
assert(string.sub("hello", 2, 4) == "ell")
assert(string.find("hello world", "world") == 7)
assert(string.gsub("aaa", "a", "b") == "bbb")
assert(string.format("%d %.2f %s", 42, 3.14, "ok") == "42 3.14 ok")
assert(string.format("%x", 255) == "ff")
assert(#("abc" .. "def") == 6)
print("String library: PASS")

-- Pattern matching
assert(string.match("2026-09-06", "(%d+)-(%d+)-(%d+)") == "2026")
local y, m, d = string.match("2026-09-06", "(%d+)-(%d+)-(%d+)")
assert(y == "2026" and m == "09" and d == "06")
local t = {}
for w in string.gmatch("one two three", "%a+") do t[#t+1] = w end
assert(#t == 3 and t[1] == "one" and t[3] == "three")
assert(string.match("hello123", "^%a+%d+$") == "hello123")
print("Pattern matching: PASS")

-- Tables and sorting
local t = {3, 1, 4, 1, 5, 9, 2, 6}
table.sort(t)
assert(t[1] == 1 and t[2] == 1 and t[8] == 9)
table.sort(t, function(a, b) return a > b end)
assert(t[1] == 9 and t[8] == 1)
local u = {10, 20, 30}
table.insert(u, 2, 15)
assert(u[2] == 15 and #u == 4)
table.remove(u, 1)
assert(u[1] == 15 and #u == 3)
assert(table.concat({"a", "b", "c"}, ",") == "a,b,c")
local moved = {1, 2, 3, 4, 5}
table.move(moved, 3, 5, 1)
assert(moved[1] == 3 and moved[2] == 4 and moved[3] == 5)
print("Tables and sorting: PASS")

-- Functions, closures, and varargs
local function adder(x) return function(y) return x + y end end
assert(adder(10)(32) == 42)
local function sum(...) local s = 0; for _, v in ipairs{...} do s = s + v end; return s end
assert(sum(1, 2, 3, 4, 5) == 15)
assert(select("#", "a", nil, "b") == 3)
assert(select(2, 10, 20, 30) == 20)
local function tail(n) if n <= 0 then return 0 end; return tail(n - 1) end
tail(100000)
print("Functions, closures, and varargs: PASS")

-- Metatables and metamethods
local vec = {}
vec.__index = vec
function vec.new(x, y) return setmetatable({x=x, y=y}, vec) end
function vec:__add(o) return vec.new(self.x + o.x, self.y + o.y) end
function vec:__tostring() return "(" .. self.x .. "," .. self.y .. ")" end
function vec:__len() return math.sqrt(self.x^2 + self.y^2) end
local a, b = vec.new(3, 0), vec.new(0, 4)
local c = a + b
assert(c.x == 3 and c.y == 4)
assert(tostring(c) == "(3,4)")
assert(math.abs(#c - 5.0) < 1e-10)
local proxy = setmetatable({}, {
    __index = function(_, k) return k:upper() end,
    __newindex = function() error("read-only") end
})
assert(proxy.hello == "HELLO")
assert(not pcall(function() proxy.x = 1 end))
print("Metatables and metamethods: PASS")

-- Coroutines
local function fib()
    local a, b = 0, 1
    while true do
        coroutine.yield(a)
        a, b = b, a + b
    end
end
local co = coroutine.create(fib)
local seq = {}
for i = 1, 10 do local _, v = coroutine.resume(co); seq[i] = v end
assert(seq[1] == 0 and seq[2] == 1 and seq[7] == 8 and seq[10] == 34)
assert(coroutine.status(co) == "suspended")
local wrap = coroutine.wrap(function()
    coroutine.yield("first")
    coroutine.yield("second")
    return "done"
end)
assert(wrap() == "first")
assert(wrap() == "second")
assert(wrap() == "done")
print("Coroutines: PASS")

-- Error handling (setjmp and longjmp)
local ok, err = pcall(function() error("test error") end)
assert(not ok)
assert(string.find(err, "test error"))
local ok2, err2 = pcall(error, 42)
assert(not ok2 and err2 == 42)
local ok3, msg3 = xpcall(
    function() error("xp") end,
    function(e) return "caught: " .. e end
)
assert(not ok3)
assert(string.find(msg3, "caught:"))
local function deep(n) if n > 0 then deep(n-1) else error("bottom") end end
local ok4, err4 = pcall(deep, 200)
assert(not ok4 and string.find(err4, "bottom"))
print("Error handling: PASS")

-- Garbage collection
collectgarbage("collect")
local before = collectgarbage("count")
local waste = {}
for i = 1, 100000 do waste[i] = {i, tostring(i)} end
local after_alloc = collectgarbage("count")
waste = nil
collectgarbage("collect")
local after_gc = collectgarbage("count")
assert(after_alloc > before)
assert(after_gc < after_alloc)
print(string.format("GC: %.0f KB -> %.0f KB -> %.0f KB", before, after_alloc, after_gc))
print("Garbage collection: PASS")

-- File I/O (the stdio layer)
local fname = "/tmp/_lua_test.txt"
local f = io.open(fname, "w")
if f then
    f:write("line1\nline2\nline3\n")
    f:close()
    local f2 = io.open(fname, "r")
    assert(f2:read("l") == "line1")
    assert(f2:read("l") == "line2")
    assert(f2:read("l") == "line3")
    f2:seek("set", 0)
    local all = f2:read("a")
    assert(#all == 18)
    f2:close()
    os.remove(fname)
    assert(io.open(fname, "r") == nil)
    print("File I/O: PASS")
else
    print("File I/O: SKIP (cannot write to " .. fname .. ")")
end

-- os library and time
local t = os.time()
assert(type(t) == "number" and t > 0)
local d = os.date("*t", t)
assert(d.year >= 2026)
assert(d.month >= 1 and d.month <= 12)
assert(d.day >= 1 and d.day <= 31)
local formatted = os.date("%Y-%m-%d", t)
assert(#formatted == 10)
assert(os.difftime(t, t) == 0)
local c = os.clock()
assert(type(c) == "number" and c >= 0)
print("os library and time: PASS")
