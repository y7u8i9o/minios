-- Lua interpreter test: the standard libraries on the minios libc.
local function fib(n) if n < 2 then return n end return fib(n - 1) + fib(n - 2) end
print("fib", fib(15))
print(string.format("float %.3f %g %a", math.pi, math.huge, 1.0))
print("math", math.floor(2.7), math.fmod(7, 3), math.sqrt(16), 7 // 2, 2 ^ 10)
print("int", math.maxinteger, math.tointeger(3.0), 1 << 40)
print("str", ("Hello"):upper(), #"minios", ("a,b,c"):gsub(",", ";"))
print("cmp", "abc" < "abd", "Z" < "a", ("%d"):format(42))
print("utf8", utf8.len("héllo"), utf8.char(0x4e2d, 0x6587))
local t = {}
for i = 1, 10 do t[#t + 1] = i * i end
print("table", table.concat(t, " "), #t)
table.sort(t, function(a, b) return a > b end)
print("sorted", t[1], t[10])
print("args", #arg, arg[1])
local ok, err = pcall(function() error("boom") end)
print("pcall", ok, err:match("boom"))
local co = coroutine.wrap(function() for i = 1, 3 do coroutine.yield(i) end end)
print("coroutine", co(), co(), co())

-- io: a temporary file, numeric reads through ungetc, seek and lines.
local f = assert(io.tmpfile())
f:write("12 3.5 0x10\nsecond line\n")
f:seek("set")
print("read numbers", f:read("n", "n", "n"))
print("read rest", f:read("l") == "", f:read("l"), f:read("l"))
f:seek("set", 3)
print("seek", f:read(3))
f:close()

local name = os.tmpname()
local g = assert(io.open(name, "w"))
g:write("alpha\nbeta\n")
g:close()
local n = 0
for line in io.lines(name) do n = n + 1 end
print("lines", n)
assert(os.remove(name))
print("removed", io.open(name) == nil)

-- A binary chunk is loaded through freopen.
local chunk = string.dump(function() return "from bytecode" end)
local h = assert(io.open("/tmp/chunk.luac", "wb"))
h:write(chunk)
h:close()
print("bytecode", dofile("/tmp/chunk.luac"))
os.remove("/tmp/chunk.luac")

-- os: clock, time, date and the shell.
print("clock", type(os.clock()), os.time() > 0, os.date("!%Y", 0))
print("execute", os.execute())
print("execute status", os.execute("exit 3"))
print("getenv", os.getenv("PATH"))
print("done")
