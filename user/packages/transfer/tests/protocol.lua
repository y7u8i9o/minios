-- Host test of mft.lua against tools/transfer.py (make check-transfer).
-- The Lua client works with the Python server, and the Python client with
-- the Lua server, in a scratch folder given as the first argument.
--
--     test_modules user/packages/transfer/tests/protocol.lua build/lua/host/transfer
local WORK = assert(arg[1], "usage: protocol.lua WORKDIR")
if WORK:sub(1, 1) ~= "/" then WORK = require("fs").getcwd() .. "/" .. WORK end
local FILES = "user/packages/transfer/files/"
package.path = FILES .. "usr/share/lua/5.5/?.lua;" .. package.path
local mft = require "mft"
local fs = require "fs"
local sys = require "sys"
local thread = require "thread"

local checks = 0
local function check(v, why)
  if not v then error("FAIL " .. why, 2) end
  checks = checks + 1
end

local function sh(cmd)
  local ok = os.execute(cmd)
  return ok == true or ok == 0
end

local function q(s) return "'" .. s:gsub("'", "'\\''") .. "'" end

local function random_bytes(n, seed)
  math.randomseed(seed)
  local parts = {}
  for i = 1, n // 8 do parts[i] = string.pack("<i8", math.random(math.mininteger, math.maxinteger)) end
  local s = table.concat(parts)
  return s .. string.rep("x", n - #s)
end

local function write(path, data)
  local f = assert(io.open(path, "wb"))
  f:write(data)
  f:close()
end

local function read(path)
  local f = io.open(path, "rb")
  if not f then return nil end
  local d = f:read("a")
  f:close()
  return d
end

sh("rm -rf " .. q(WORK))
assert(fs.mkdir(WORK))
local A, B, L = WORK .. "/py-root", WORK .. "/lua-root", WORK .. "/local"
for _, d in ipairs({ A, B, L, A .. "/tree", A .. "/tree/empty", A .. "/tree/sub" }) do assert(fs.mkdir(d)) end
local big = random_bytes(3000000, 7)
write(A .. "/big name with spaces.bin", big)
write(A .. "/small.txt", "hello from python\n")
write(A .. "/empty", "")
write(A .. "/tree/sub/leaf.txt", "leaf\n")

-- ---- paths ----

check(mft.encode("a b/c%d") == "a%20b/c%25d", "encode")
check(mft.decode("a%20b/c%25d") == "a b/c%d", "decode")
check(mft.decode(".") == ".", "root")
for _, bad in ipairs({ "", "/abs", "a/../b", "a//b", "./a", "a/", "%zz", "a%2" }) do
  check(mft.decode(bad) == nil, "rejects " .. bad)
end
check(mft.part_name("x y", 10, 99) == ".x y.10-99.mft-part", "part name")
check(select(2, mft.parse_part(".a.b.5-7.mft-part")) == 5, "part size")
check(mft.parse_part(".a.b.5-7.mft-part") == "a.b", "part name with dots")
local uris = mft.uri_paths("# comment\r\nfile:///tmp/a%20b\r\nfile://host/x\r\nhttp://e/f\r\n")
check(#uris == 2 and uris[1] == "/tmp/a b" and uris[2] == "/x", "uri list")
check(mft.file_uri("/a b") == "file:///a%20b", "file uri")

-- ---- the Python server ----

local server = assert(io.popen("sh -c 'echo $$; exec python3 tools/transfer.py serve -p 0 " .. q(A) .. "'"))
local pypid = math.tointeger(tonumber(server:read("l")))
local serving = server:read("l")
local pyport = math.tointeger(tonumber(serving and serving:match("on port (%d+)$")))
check(pypid and pyport, "python server started: " .. tostring(serving))
-- The body runs under xpcall. The server is stopped after a failure as
-- well, because closing the pipe of io.popen waits for its process.
local function body()

  local c = assert(mft.connect("127.0.0.1", pyport))
  check(c.name == "py-root", "server name " .. tostring(c.name))
  local entries = assert(c:list("."))
  local byname = {}
  for _, e in ipairs(entries) do byname[e.name] = e end
  check(byname["big name with spaces.bin"].size == 3000000, "list size")
  check(byname.tree.kind == "d", "list folder")
  local st = assert(c:stat("small.txt"))
  check(st.kind == "f" and st.size == 18, "stat")
  local _, _, code = c:stat("missing")
  check(code == "ENOENT", "stat ENOENT")

  -- downloads
  local e = byname["big name with spaces.bin"]
  check(c:get("big name with spaces.bin", e.size, e.mtime, L, e.name) == 3000000, "get big")
  check(read(L .. "/" .. e.name) == big, "get big content")
  check(fs.stat(L .. "/" .. e.name).mtime == e.mtime, "get mtime")
  check(c:get("empty", 0, byname.empty.mtime, L, "empty") == 0 and read(L .. "/empty") == "", "get empty")
  _, _, code = c:get("small.txt", 18, byname["small.txt"].mtime + 1, L, "small.txt")
  check(code == "ESTALE", "get ESTALE")
  check(not fs.exists(L .. "/" .. mft.part_name("small.txt", 18, byname["small.txt"].mtime + 1)), "no part after ESTALE")

  -- a resumed download
  write(L .. "/" .. mft.part_name("again.bin", 3000000, e.mtime), big:sub(1, 1000000))
  write(L .. "/" .. mft.part_name("again.bin", 5, 5), "stale")
  local first
  check(c:get("big name with spaces.bin", e.size, e.mtime, L, "again.bin", {
    progress = function(done) first = first or done end }) == 2000000, "get resumed")
  check(first == 1000000 and read(L .. "/again.bin") == big, "get resumed content")
  check(not fs.exists(L .. "/" .. mft.part_name("again.bin", 5, 5)), "stale part removed")

  -- a cancelled download, then a resumed one
  local got = 0
  local ok
  ok, _, code = c:get("big name with spaces.bin", e.size, e.mtime, L, "cancel.bin", {
    progress = function(done) got = done end,
    cancelled = function() return got >= 300000 end,
  })
  check(not ok and code == "ECANCELED", "get cancelled")
  local part = fs.stat(L .. "/" .. mft.part_name("cancel.bin", e.size, e.mtime))
  check(part and part.size >= 300000 and part.size < 3000000, "part after cancel")
  check(c:list("."), "session usable after a cancel")
  check(c:get("big name with spaces.bin", e.size, e.mtime, L, "cancel.bin") == 3000000 - part.size, "get after cancel")
  check(read(L .. "/cancel.bin") == big, "content after cancel")

  -- a folder
  local plan = assert(mft.download_plan(c, "tree", "d", 0, byname.tree.mtime, L))
  for _, step in ipairs(plan) do check(mft.run_step(c, "get", step), "run step " .. step.dst) end
  check(fs.stat(L .. "/tree/empty").type == "directory", "empty folder downloaded")
  check(read(L .. "/tree/sub/leaf.txt") == "leaf\n", "tree file downloaded")

  -- uploads
  write(L .. "/up.txt", "from lua\n")
  check(c:put(L .. "/up.txt", "up.txt") == 9 and read(A .. "/up.txt") == "from lua\n", "put")
  check(fs.stat(A .. "/up.txt").mtime == fs.stat(L .. "/up.txt").mtime, "put mtime")
  local lst = fs.stat(L .. "/again.bin")
  write(A .. "/" .. mft.part_name("resumed.bin", lst.size, lst.mtime), big:sub(1, 2500000))
  first = nil
  check(c:put(L .. "/again.bin", "resumed.bin", { progress = function(done) first = first or done end }) == 500000,
        "put resumed")
  check(first == 2500000 and read(A .. "/resumed.bin") == big, "put resumed content")
  got = 0
  ok, _, code = c:put(L .. "/again.bin", "cancelled.bin", {
    progress = function(done) got = done end,
    cancelled = function() return got >= 200000 end,
  })
  check(not ok and code == "ECANCELED", "put cancelled")
  check(c:put(L .. "/again.bin", "cancelled.bin") < 3000000 and read(A .. "/cancelled.bin") == big, "put after cancel")
  plan = assert(mft.upload_plan(L .. "/tree", "."))
  local copied = {}
  for _, step in ipairs(plan) do
    check(mft.run_step(c, "put", step), "upload step " .. step.dst)
    copied[step.dst] = true
  end
  check(copied["tree/empty"] and copied["tree/sub/leaf.txt"], "upload plan")
  plan = assert(mft.upload_plan(L .. "/tree", "."))
  for _, step in ipairs(plan) do check(mft.run_step(c, "put", step), "repeated upload " .. step.dst) end

  -- file management
  check(c:mkdir("made"), "mkdir")
  _, _, code = c:mkdir("made")
  check(code == "EEXIST", "mkdir EEXIST")
  _, _, code = c:rename("made", "small.txt")
  check(code == "EEXIST", "rename does not replace")
  check(c:rename("made", "renamed dir"), "rename")
  check(fs.stat(A .. "/renamed dir").type == "directory", "renamed")
  check(c:delete("tree"), "delete tree")
  check(not fs.exists(A .. "/tree"), "deleted tree")
  _, _, code = c:delete("../x")
  check(code == "EINVAL", "unsafe path refused")
  c:quit()

  -- ---- the Lua server ----

  write(B .. "/lua.txt", "from the lua server\n")
  write(B .. "/big.bin", big)
  assert(fs.mkdir(B .. "/folder"))
  assert(fs.mkdir(B .. "/folder/empty"))
  local srv = assert(thread.spawn(FILES .. "usr/share/apps/transfer/server.lua", "0\n" .. B))
  local first_msg = srv:receive(5000)
  local luaport = math.tointeger(tonumber(first_msg and first_msg:match("^listening (%d+)$")))
  check(luaport, "lua server listening: " .. tostring(first_msg))
  local out = WORK .. "/py-out"
  assert(fs.mkdir(out))
  local function py(args)
    return sh("python3 tools/transfer.py " .. args .. " > " .. q(WORK .. "/py.log") .. " 2>&1")
  end
  check(py("ls -p " .. luaport), "python ls")
  local listing = read(WORK .. "/py.log")
  check(listing:find("lua.txt", 1, true) and listing:find("folder/", 1, true), "python listing")
  check(py("get -p " .. luaport .. " -o " .. q(out) .. " big.bin folder lua.txt"), "python get")
  check(read(out .. "/big.bin") == big and fs.stat(out .. "/folder/empty"), "python get content")
  check(py("put -p " .. luaport .. " -d folder " .. q(L .. "/again.bin") .. " " .. q(L .. "/up.txt")), "python put")
  check(read(B .. "/folder/again.bin") == big and read(B .. "/folder/up.txt") == "from lua\n", "python put content")
  write(B .. "/folder/" .. mft.part_name("up2.bin", lst.size, lst.mtime), big:sub(1, 1234567))
  sh("cp -p " .. q(L .. "/again.bin") .. " " .. q(L .. "/up2.bin"))
  check(py("put -p " .. luaport .. " -d folder " .. q(L .. "/up2.bin")), "python resumed put")
  check(read(B .. "/folder/up2.bin") == big, "python resumed put content")
  check(py("mkdir -p " .. luaport .. " newdir") and fs.stat(B .. "/newdir"), "python mkdir")
  check(py("mv -p " .. luaport .. " newdir moved") and fs.stat(B .. "/moved"), "python mv")
  check(py("rm -p " .. luaport .. " moved folder") and not fs.exists(B .. "/folder"), "python rm")
  check(not py("get -p " .. luaport .. " -o " .. q(out) .. " missing"), "python get missing fails")
  check(read(WORK .. "/py.log"):find("(ENOENT)", 1, true), "ENOENT reported")

  -- the log lines of the Lua server
  local lines = {}
  while true do
    local m = srv:receive(200)
    if not m then break end
    lines[#lines + 1] = m
  end
  local log = table.concat(lines, "\n")
  check(log:find("log sent big.bin, 3000000 bytes", 1, true), "lua server sent line")
  check(log:find("log stored folder/up2.bin, 3000000 bytes, resumed at 1234567", 1, true), "lua server resumed line")

  -- the Lua client with the Lua server
  local lc = assert(mft.connect("127.0.0.1", luaport))
  check(lc.name == "lua-root", "lua server name")
  check(lc:put(L .. "/up.txt", "up.txt") == 9 and read(B .. "/up.txt") == "from lua\n", "lua to lua put")
  local lst2 = assert(lc:stat("big.bin"))
  check(lc:get("big.bin", lst2.size, lst2.mtime, L, "lua-big.bin") == 3000000 and read(L .. "/lua-big.bin") == big,
        "lua to lua get")
  got = 0
  ok, _, code = lc:get("big.bin", lst2.size, lst2.mtime, L, "lua-cancel.bin", {
    progress = function(done) got = done end,
    cancelled = function() return got >= 100000 end,
  })
  check(not ok and code == "ECANCELED" and lc:list("."), "lua server cancel")
  lc:quit()
  srv:stop()
  check(srv:join(), "lua server ended")
  srv:close()
end

local ok, err = xpcall(body, debug.traceback)
sys.kill(pypid, "TERM")
server:close()
if not ok then error(err, 0) end
sh("rm -rf " .. q(WORK))
print("transfer: " .. checks .. " protocol checks passed")
