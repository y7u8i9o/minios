-- Test of the fs and sys modules. arg[1] is a writable scratch
-- directory. On the host (make check-lua) arg[2] and arg[3] name the
-- MIME tables. On minios the lua_sys boot test passes --no-init as
-- arg[2]: the kernel run test starts the program without init, so a
-- process spawned under init would never be reaped and would count as
-- leaked pages.
local fs = require "fs"
local sys = require "sys"
local scratch = assert(arg[1], "scratch directory argument")
local no_init = arg[2] == "--no-init"
if arg[2] and not no_init then assert(sys.mime_load(arg[2], arg[3])) end

local function check(cond, name)
  if cond then print("ok " .. name) else error("failed: " .. name, 2) end
end

-- directories
local dir = scratch .. "/modtest"
check(fs.mkdir(dir), "mkdir")
check(fs.exists(dir) and fs.stat(dir).type == "directory", "stat directory")
check(fs.write(dir .. "/b.txt", "hello\n"), "write")
check(fs.write(dir .. "/a.txt", "12345"), "write second")
check(fs.write(dir .. "/a.txt", "67", true), "append")
check(fs.read(dir .. "/a.txt") == "1234567", "read back")
local st = fs.stat(dir .. "/a.txt")
check(st.type == "file" and st.size == 7 and st.mtime > 0, "stat file")
check(fs.mkdir(dir .. "/sub"), "mkdir nested")
local names = fs.list(dir)
check(#names == 3 and names[1] == "a.txt" and names[2] == "b.txt" and names[3] == "sub", "list sorted")
local seen, dirs = 0, 0
for name, kind in fs.dir(dir) do
  seen = seen + 1
  if kind == "directory" then dirs = dirs + 1 end
end
check(seen == 3 and dirs == 1, "dir iterator")
local ok, err, code = fs.rmdir(dir)
check(ok == nil and type(err) == "string" and code > 0, "rmdir refuses a full directory")
check(fs.rmdir(dir .. "/sub") and os.remove(dir .. "/a.txt") and os.remove(dir .. "/b.txt"), "remove contents")
check(fs.rmdir(dir) and not fs.exists(dir), "rmdir")
local ok2, err2 = fs.stat(dir)
check(ok2 == nil and err2 ~= nil, "stat missing")
check(not pcall(fs.dir, dir), "dir missing raises")
local cwd = fs.getcwd()
check(fs.chdir(scratch) and fs.getcwd() ~= "" and fs.chdir(cwd) and fs.getcwd() == cwd, "chdir round trip")

-- processes
check(sys.pid() > 0 and sys.ppid() >= 0, "pid")
local ran, what, code2 = sys.run("sh", "-c", "exit 0")
check(ran == true and what == "exit" and code2 == 0, "run success")
ran, what, code2 = sys.run("sh", "-c", "exit 3")
check(ran == nil and what == "exit" and code2 == 3, "run exit status")
if no_init then
  print("ok spawn skipped without init")
else
  local marker = scratch .. "/spawned"
  check(sys.spawn("sh", "-c", "echo spawned > " .. marker), "spawn")
  local waited = 0
  while not fs.exists(marker) and waited < 5000 do sys.sleep(20); waited = waited + 20 end
  check(fs.read(marker) == "spawned\n", "spawn ran under init")
  os.remove(marker)
end
check(not pcall(sys.kill, sys.pid(), "NOSUCH"), "kill rejects an unknown name")
local before = sys.uptime()
sys.sleep(15)
check(sys.uptime() - before >= 10, "uptime and sleep")
sys.yield()

-- system data
local u = sys.uname()
check(type(u.sysname) == "string" and #u.sysname > 0 and type(u.machine) == "string", "uname")
check(sys.nproc() >= 1 and sys.cpu() >= 0, "processors")
check(sys.type("script.lua") == "text/x-lua", "mime type")
check(sys.type(scratch) == "inode/directory", "mime directory")
check(sys.handler("text/x-lua") == "/bin/code", "mime handler")
check(sys.handler("inode/directory") == "/bin/files", "mime handler exact")
print("modules: done")
