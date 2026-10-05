-- mft: the file transfer protocol MFT 1 (docs/design/filetransfer.md).
-- The module offers the client (mft.connect), the plans of folder copies,
-- one server session (mft.serve_session) and the accept loop of a server
-- (mft.serve). Failures return nil, a message and the POSIX name of the
-- error, for example nil, "No such file or directory", "ENOENT".
--
-- Sockets come from the net module and block. The client waits at most
-- STALL_MS for the server at each step. A server session waits for the
-- next request without a limit, in steps of IDLE_MS, and asks
-- opts.stopped() between the steps.

local net = require "net"
local sys = require "sys"
local fs = require "fs"

local mft = {}

mft.VERSION = 1
mft.CHUNK = 65536
mft.PART_SUFFIX = ".mft-part"
mft.CLIENT_PORT = 9103          -- the host server, reached at 10.0.2.2
mft.SERVE_PORT = 9102           -- this server, forwarded by tools/run.sh
mft.DEFAULT_HOST = "10.0.2.2"

local STALL_MS = 30000
local IDLE_MS = 250
local MAX_LINE = 8192
local MAX_PATH = 1024

-- ---- errors ----

local codes = {
  "ENOENT", "EEXIST", "EACCES", "EINVAL", "EIO", "ENOSPC", "ENOTDIR", "EISDIR",
  "ENOTEMPTY", "ENAMETOOLONG", "ESTALE", "ECANCELED", "EPROTO",
}
local known = {}
for _, c in ipairs(codes) do known[c] = true end

-- The POSIX name of an errno number, or EIO for a number without a name
-- in sys.errno. A local failure, such as a refused connection, carries its
-- own name. An error line carries only the codes of the protocol
-- (wire_code).
local names = {}
for name, number in pairs(sys.errno) do names[number] = name end

function mft.code_of(errno)
  return names[errno] or "EIO"
end

function mft.wire_code(code)
  return known[code] and code or "EIO"
end

-- Converts the results nil, message, errno of a library call.
local function fail(msg, errno)
  return nil, msg or "unknown error", type(errno) == "number" and mft.code_of(errno) or errno or "EIO"
end

-- ---- paths ----

function mft.encode(path)
  return (path:gsub("[^A-Za-z0-9%-%._~/]", function(c) return string.format("%%%02X", c:byte()) end))
end

-- Reverses the percent encoding without a check of the result. A
-- malformed escape returns nil.
function mft.unescape(text)
  if text:gsub("%%%x%x", ""):find("%", 1, true) then return nil end
  return (text:gsub("%%(%x%x)", function(h) return string.char(tonumber(h, 16)) end))
end

-- The file URI of an absolute local path, as text/uri-list carries it
-- (docs/design/dnd.md).
function mft.file_uri(path)
  return "file://" .. mft.encode(path)
end

-- The local paths of a text/uri-list. Lines of other schemes and comments
-- are skipped.
function mft.uri_paths(list)
  local paths = {}
  for line in list:gmatch("[^\r\n]+") do
    local rest = line:match("^file://(.*)$")
    local path = rest and mft.unescape((rest:gsub("^[^/]*", "", 1)))
    if path and path ~= "" then paths[#paths + 1] = path end
  end
  return paths
end

-- Decodes and checks a path of the wire. The root folder is ".".
function mft.decode(text)
  if not text or text == "" then return nil, "empty path", "EINVAL" end
  local path = mft.unescape(text)
  if not path then return nil, "bad percent encoding", "EINVAL" end
  local ok, msg, code = mft.check(path)
  if not ok then return nil, msg, code end
  return path
end

function mft.check(path)
  if #path > MAX_PATH then return nil, "path too long", "ENAMETOOLONG" end
  if path == "." then return true end
  if path == "" or path:sub(1, 1) == "/" or path:sub(-1) == "/" or path:find("\0", 1, true) then
    return nil, "invalid path", "EINVAL"
  end
  for part in (path .. "/"):gmatch("(.-)/") do
    if part == "" or part == "." or part == ".." then return nil, "invalid path", "EINVAL" end
  end
  return true
end

-- Joins a remote folder and a name. The root folder is ".".
function mft.join(dir, name)
  if dir == "." or dir == "" then return name end
  return dir .. "/" .. name
end

-- Joins a local folder and a name.
function mft.local_join(dir, name)
  if dir:sub(-1) == "/" then return dir .. name end
  return dir .. "/" .. name
end

function mft.basename(path)
  local trimmed = path:gsub("/+$", "")
  if trimmed == "" then return "/" end
  return trimmed:match("([^/]*)$")
end

function mft.dirname(path)
  local dir = path:gsub("/+$", ""):match("^(.*)/[^/]*$")
  if dir == nil then return "." end
  if dir == "" then return "/" end
  return dir
end

-- ---- part files ----

function mft.part_name(name, size, mtime)
  return string.format(".%s.%d-%d%s", name, size, mtime, mft.PART_SUFFIX)
end

-- The name, the size and the modification time of a part file, or nil.
function mft.parse_part(file)
  local name, size, mtime = file:match("^%.(.+)%.(%d+)%-(%-?%d+)%.mft%-part$")
  if not name then return nil end
  return name, math.tointeger(tonumber(size)), math.tointeger(tonumber(mtime))
end

-- A name of a part file, as the listings hide it: a leading dot and the
-- suffix .mft-part, as in tools/mft.py.
function mft.is_part(file)
  return file:sub(1, 1) == "." and #file > #mft.PART_SUFFIX and file:sub(-#mft.PART_SUFFIX) == mft.PART_SUFFIX
end

-- prepare_part removes the part files of name in dir that belong to
-- another version of the file. Returns the path of the part file and the
-- length to resume from.
local function prepare_part(dir, name, size, mtime)
  local part = mft.part_name(name, size, mtime)
  for _, file in ipairs(fs.list(dir) or {}) do
    local other = mft.parse_part(file)
    if other == name and file ~= part then os.remove(mft.local_join(dir, file)) end
  end
  local path = mft.local_join(dir, part)
  local st = fs.stat(path)
  local offset = st and st.type == "file" and st.size or 0
  if offset > size then
    os.remove(path)
    offset = 0
  end
  return path, offset
end
mft.prepare_part = prepare_part

-- ---- the connection ----

local Conn = {}
Conn.__index = Conn

function mft.wrap(fd)
  return setmetatable({ fd = fd, buf = "", pos = 1 }, Conn)
end

function Conn:fill(timeout)
  local data, msg, errno = net.recv(self.fd, 65536, timeout or STALL_MS)
  if data == nil then
    if msg then return fail(msg, errno) end
    return nil, "connection closed", "ECONNRESET"
  end
  if self.pos > #self.buf then
    self.buf = data
  else
    self.buf = self.buf:sub(self.pos) .. data
  end
  self.pos = 1
  return true
end

function Conn:pending()
  return self.pos <= #self.buf
end

-- A line without its LF.
function Conn:line(timeout)
  while true do
    local e = self.buf:find("\n", self.pos, true)
    if e then
      local line = self.buf:sub(self.pos, e - 1)
      self.pos = e + 1
      return line
    end
    if #self.buf - self.pos + 1 > MAX_LINE then return nil, "line too long", "EPROTO" end
    local ok, msg, code = self:fill(timeout)
    if not ok then return nil, msg, code end
  end
end

-- Exactly n bytes.
function Conn:read(n, timeout)
  local parts, need = {}, n
  while need > 0 do
    if self.pos > #self.buf then
      local ok, msg, code = self:fill(timeout)
      if not ok then return nil, msg, code end
    end
    local take = math.min(need, #self.buf - self.pos + 1)
    parts[#parts + 1] = self.buf:sub(self.pos, self.pos + take - 1)
    self.pos = self.pos + take
    need = need - take
  end
  return table.concat(parts)
end

-- True when input waits, without blocking.
function Conn:readable()
  if self:pending() then return true end
  local entries = { { fd = self.fd, events = sys.POLLIN } }
  local n = sys.poll(entries, 0)
  return n and n > 0
end

function Conn:send(text)
  local ok, msg, errno = net.send(self.fd, text, STALL_MS)
  if not ok then return fail(msg, errno) end
  return true
end

function Conn:close()
  if self.fd then
    sys.close(self.fd)
    self.fd = nil
  end
end

local function split(line)
  local words = {}
  for w in line:gmatch("%S+") do words[#words + 1] = w end
  return words
end

local function int(text)
  local n = text and text:match("^%-?%d+$") and math.tointeger(tonumber(text))
  return n
end

-- The error of an ERR line.
local function err_of(line)
  local code, msg = line:match("^ERR (%S+) ?(.*)$")
  if not code then return nil, "unexpected reply: " .. line:sub(1, 80), "EPROTO" end
  if not known[code] then code = "EIO" end
  return nil, msg ~= "" and msg or code, code
end

local function chunk_header(data)
  return string.format("C %d %08x\n", #data, sys.crc32(data))
end

-- ---- the client ----

local Client = {}
Client.__index = Client

-- mft.connect(host, port [, timeout_ms]) -> client. client.name is the
-- name of the served folder.
function mft.connect(host, port, timeout)
  local fd, msg, errno = net.connect(host, port, timeout or 5000)
  if not fd then return fail(msg, errno) end
  local c = mft.wrap(fd)
  local ok, smsg, code = c:send("MFT 1\n")
  local line = ok and c:line()
  if not line then
    c:close()
    return nil, smsg or "no answer", code or "EPROTO"
  end
  local version, name = line:match("^MFT (%d+) ?(.*)$")
  if version ~= "1" then
    c:close()
    if line:match("^ERR ") then return err_of(line) end
    return nil, "not an MFT server", "EPROTO"
  end
  local self = setmetatable({ conn = c, host = host, port = port }, Client)
  self.name = name ~= "" and (mft.unescape(name) or name) or host
  return self
end

-- Sends a request and returns the words of its OK line.
function Client:request(line)
  local ok, msg, code = self.conn:send(line .. "\n")
  if not ok then return nil, msg, code end
  local reply
  reply, msg, code = self.conn:line()
  if not reply then return nil, msg, code end
  if reply:match("^OK") then return split(reply) end
  return err_of(reply)
end

-- list(path) -> array of { name, kind = "d" | "f", size, mtime }
function Client:list(path)
  local words, msg, code = self:request("LIST " .. mft.encode(path))
  if not words then return nil, msg, code end
  local count = int(words[2])
  if not count then return nil, "bad LIST reply", "EPROTO" end
  local entries = {}
  for _ = 1, count do
    local line
    line, msg, code = self.conn:line()
    if not line then return nil, msg, code end
    local kind, size, mtime, name = line:match("^([df]) (%d+) (%-?%d+) (%S+)$")
    name = name and mft.decode(name)
    if not name then return nil, "bad LIST entry", "EPROTO" end
    entries[#entries + 1] = { name = name, kind = kind, size = int(size), mtime = int(mtime) }
  end
  return entries
end

function Client:stat(path)
  local words, msg, code = self:request("STAT " .. mft.encode(path))
  if not words then return nil, msg, code end
  if (words[2] ~= "d" and words[2] ~= "f") or not int(words[3]) or not int(words[4]) then
    return nil, "bad STAT reply", "EPROTO"
  end
  return { kind = words[2], size = int(words[3]), mtime = int(words[4]) }
end

function Client:mkdir(path)
  local words, msg, code = self:request("MKDIR " .. mft.encode(path))
  if not words then return nil, msg, code end
  return true
end

function Client:rename(from, to)
  local words, msg, code = self:request("RENAME " .. mft.encode(from) .. " " .. mft.encode(to))
  if not words then return nil, msg, code end
  return true
end

function Client:delete(path)
  local words, msg, code = self:request("DELETE " .. mft.encode(path))
  if not words then return nil, msg, code end
  return true
end

function Client:quit()
  if self.conn.fd then self:request("QUIT") end
  self:close()
end

function Client:close()
  self.conn:close()
end

local function call(fn, ...)
  if fn then return fn(...) end
end

-- get(remote, size, mtime, local_dir, local_name [, opts]) downloads a
-- file into local_dir. opts.progress(done, size) reports the bytes of
-- the file, opts.cancelled() ends the transfer when it returns true.
-- Returns the number of bytes received in this call.
function Client:get(remote, size, mtime, dir, name, opts)
  opts = opts or {}
  local part, offset = prepare_part(dir, name, size, mtime)
  local f, ferr, ferrno = io.open(part, "ab")
  if not f then return fail(ferr, ferrno) end
  local words, msg, code = self:request(string.format("GET %s %d %d %d", mft.encode(remote), offset, size, mtime))
  if not words then
    f:close()
    if offset == 0 then os.remove(part) end
    return nil, msg, code
  end
  local c, done, received = self.conn, offset, 0
  local bad, cancelling, failure
  call(opts.progress, done, size)
  while true do
    local line
    line, msg, code = c:line()
    if not line then
      f:close()
      return nil, msg, code
    end
    local len, crc = line:match("^C (%d+) (%x+)$")
    if len then
      local data
      data, msg, code = c:read(int(len))
      if not data then
        f:close()
        return nil, msg, code
      end
      if not cancelling and not bad and not failure then
        if sys.crc32(data) ~= tonumber(crc, 16) then
          bad = true
        elseif done + #data > size then
          failure = { "the server sent more than the size", "EPROTO" }
        else
          local ok, werr, werrno = f:write(data)
          if not ok then
            failure = { werr, mft.code_of(werrno) }
          else
            done = done + #data
            received = received + #data
            call(opts.progress, done, size)
          end
        end
        if not cancelling and opts.cancelled and opts.cancelled() then
          cancelling = true
          c:send("A\n")
        end
      end
    elseif line == "E" or line == "A" then
      break
    elseif line:match("^X ") then
      f:close()
      return err_of("ERR " .. line:sub(3))
    else
      f:close()
      return nil, "unexpected line in the data", "EPROTO"
    end
  end
  f:close()
  if cancelling then return nil, "cancelled", "ECANCELED" end
  if bad then return nil, "checksum mismatch", "EIO" end
  if failure then return nil, failure[1], failure[2] end
  if done ~= size then return nil, "the data ended early", "EIO" end
  fs.utime(part, mtime)
  local ok, rerr, rerrno = os.rename(part, mft.local_join(dir, name))
  if not ok then return fail(rerr, rerrno) end
  return received
end

-- put(local_path, remote [, opts]) uploads a file to the remote path.
-- opts as for get. Returns the number of bytes sent in this call.
function Client:put(path, remote, opts)
  opts = opts or {}
  local st, serr, serrno = fs.stat(path)
  if not st then return fail(serr, serrno) end
  if st.type ~= "file" then return nil, path .. ": not a file", "EINVAL" end
  local f, ferr, ferrno = io.open(path, "rb")
  if not f then return fail(ferr, ferrno) end
  local words, msg, code = self:request(string.format("PUT %s %d %d", mft.encode(remote), st.size, st.mtime))
  if not words then
    f:close()
    return nil, msg, code
  end
  local offset = int(words[2])
  if not offset or offset < 0 or offset > st.size then
    f:close()
    return nil, "bad PUT reply", "EPROTO"
  end
  f:seek("set", offset)
  local c, done = self.conn, offset
  call(opts.progress, done, st.size)
  while done < st.size do
    if opts.cancelled and opts.cancelled() then
      f:close()
      local ok
      ok, msg, code = c:send("A\n")
      if not ok then return nil, msg, code end
      local reply = c:line()
      if reply and not reply:match("^ERR ECANCELED") and not reply:match("^OK") then
        return nil, "unexpected reply to a cancel", "EPROTO"
      end
      return nil, "cancelled", "ECANCELED"
    end
    local data = f:read(math.min(mft.CHUNK, st.size - done))
    if not data or #data == 0 then
      -- The file became shorter. The server rejects the size.
      break
    end
    local ok
    ok, msg, code = c:send(chunk_header(data) .. data)
    if not ok then
      f:close()
      return nil, msg, code
    end
    done = done + #data
    call(opts.progress, done, st.size)
  end
  f:close()
  local ok
  ok, msg, code = c:send("E\n")
  if not ok then return nil, msg, code end
  local reply
  reply, msg, code = c:line()
  if not reply then return nil, msg, code end
  if not reply:match("^OK") then return err_of(reply) end
  return done - offset
end

-- ---- plans of copies ----

-- A plan is a list of steps { kind = "d" | "f", src, dst, name, size,
-- mtime }. A step "d" creates the folder dst, a step "f" copies the file
-- src to dst. Folders come before their contents.

-- download_plan(client, remote, kind, size, mtime, local_dir) lists the
-- remote folders of the copy.
function mft.download_plan(client, remote, kind, size, mtime, dir)
  local plan = {}
  local function add(rpath, k, sz, mt, ldir)
    local name = mft.basename(rpath)
    local dst = mft.local_join(ldir, name)
    if k == "f" then
      plan[#plan + 1] = { kind = "f", src = rpath, dst = dst, dir = ldir, name = name, size = sz, mtime = mt }
      return true
    end
    plan[#plan + 1] = { kind = "d", src = rpath, dst = dst, name = name, size = 0, mtime = mt }
    local entries, msg, code = client:list(rpath)
    if not entries then return nil, msg, code end
    for _, e in ipairs(entries) do
      local ok
      ok, msg, code = add(mft.join(rpath, e.name), e.kind, e.size, e.mtime, dst)
      if not ok then return nil, msg, code end
    end
    return true
  end
  local ok, msg, code = add(remote, kind, size, mtime, dir)
  if not ok then return nil, msg, code end
  return plan
end

-- upload_plan(local_path, remote_dir) walks the local folders of the copy.
function mft.upload_plan(path, rdir)
  local plan = {}
  local function add(lpath, rparent)
    local st, msg, errno = fs.stat(lpath)
    if not st then return fail(msg, errno) end
    local name = mft.basename(lpath)
    local dst = mft.join(rparent, name)
    if st.type == "file" then
      plan[#plan + 1] = { kind = "f", src = lpath, dst = dst, name = name, size = st.size, mtime = st.mtime }
      return true
    end
    if st.type ~= "directory" then return true end
    plan[#plan + 1] = { kind = "d", src = lpath, dst = dst, name = name, size = 0, mtime = st.mtime }
    local names
    names, msg, errno = fs.list(lpath)
    if not names then return fail(msg, errno) end
    for _, n in ipairs(names) do
      if not mft.is_part(n) then
        local ok, m, c = add(mft.local_join(lpath, n), dst)
        if not ok then return nil, m, c end
      end
    end
    return true
  end
  local ok, msg, code = add(path, rdir)
  if not ok then return nil, msg, code end
  return plan
end

function mft.plan_bytes(plan)
  local total = 0
  for _, step in ipairs(plan) do total = total + step.size end
  return total
end

-- run_step performs one step of a plan in the direction "get" or "put".
-- An existing folder is no error. Returns the bytes moved.
function mft.run_step(client, direction, step, opts)
  if step.kind == "d" then
    if direction == "get" then
      local ok, msg, errno = fs.mkdir(step.dst)
      if not ok and errno ~= sys.errno.EEXIST then return fail(msg, errno) end
    else
      local ok, msg, code = client:mkdir(step.dst)
      if not ok and code ~= "EEXIST" then return nil, msg, code end
    end
    return 0
  end
  if direction == "get" then
    return client:get(step.src, step.size, step.mtime, step.dir, step.name, opts)
  end
  return client:put(step.src, step.dst, opts)
end

-- ---- the server ----

local function remove_tree(path)
  local st, msg, errno = fs.lstat(path)
  if not st then return fail(msg, errno) end
  if st.type == "directory" then
    local names
    names, msg, errno = fs.list(path)
    if not names then return fail(msg, errno) end
    for _, n in ipairs(names) do
      local ok, m, c = remove_tree(mft.local_join(path, n))
      if not ok then return nil, m, c end
    end
    local ok
    ok, msg, errno = fs.rmdir(path)
    if not ok then return fail(msg, errno) end
    return true
  end
  local ok
  ok, msg, errno = os.remove(path)
  if not ok then return fail(msg, errno) end
  return true
end
mft.remove_tree = remove_tree

local function err_line(msg, code)
  return string.format("ERR %s %s\n", mft.wire_code(code), ((msg or "error"):gsub("\n", " ")))
end

local Session = {}
Session.__index = Session

-- The local path and the decoded path of a path of the wire.
function Session:path(text)
  local path, msg, code = mft.decode(text)
  if not path then return nil, msg, code end
  if path == "." then return self.root end
  return mft.local_join(self.root, path), path
end

function Session:log(line)
  call(self.opts.log, line)
end

function Session:list(text)
  local dir, msg, code = self:path(text)
  if not dir then return err_line(msg, code) end
  local st, serr, serrno = fs.stat(dir)
  if not st then return err_line(serr, mft.code_of(serrno)) end
  if st.type ~= "directory" then return err_line("not a folder", "ENOTDIR") end
  local names, lerr, lerrno = fs.list(dir)
  if not names then return err_line(lerr, mft.code_of(lerrno)) end
  local lines = {}
  for _, n in ipairs(names) do
    if not mft.is_part(n) then
      local est = fs.stat(mft.local_join(dir, n))
      if est and (est.type == "file" or est.type == "directory") then
        lines[#lines + 1] = string.format("%s %d %d %s\n", est.type == "file" and "f" or "d",
                                          est.type == "file" and est.size or 0, est.mtime, mft.encode(n))
      end
    end
  end
  return string.format("OK %d\n", #lines) .. table.concat(lines)
end

function Session:stat(text)
  local path, msg, code = self:path(text)
  if not path then return err_line(msg, code) end
  local st, serr, serrno = fs.stat(path)
  if not st then return err_line(serr, mft.code_of(serrno)) end
  if st.type ~= "file" and st.type ~= "directory" then return err_line("not a file or folder", "EINVAL") end
  return string.format("OK %s %d %d\n", st.type == "file" and "f" or "d", st.type == "file" and st.size or 0, st.mtime)
end

function Session:mkdir(text)
  local path, msg, code = self:path(text)
  if not path then return err_line(msg, code) end
  if path == self.root then return err_line("the root folder exists", "EEXIST") end
  local ok, merr, merrno = fs.mkdir(path)
  if not ok then return err_line(merr, mft.code_of(merrno)) end
  return "OK\n"
end

function Session:rename(from_text, to_text)
  local from, msg, code = self:path(from_text)
  if not from then return err_line(msg, code) end
  local to
  to, msg, code = self:path(to_text or "")
  if not to then return err_line(msg, code) end
  if from == self.root or to == self.root then return err_line("the root folder cannot be renamed", "EINVAL") end
  if not fs.stat(from) then return err_line("No such file or directory", "ENOENT") end
  if fs.lstat(to) then return err_line("the target exists", "EEXIST") end
  local ok, rerr, rerrno = os.rename(from, to)
  if not ok then return err_line(rerr, mft.code_of(rerrno)) end
  return "OK\n"
end

function Session:delete(text)
  local path, msg, code = self:path(text)
  if not path then return err_line(msg, code) end
  if path == self.root then return err_line("the root folder cannot be deleted", "EINVAL") end
  local ok
  ok, msg, code = remove_tree(path)
  if not ok then return err_line(msg, code) end
  return "OK\n"
end

-- Sends a file from offset. Returns false when the connection failed.
function Session:get(text, offset, size, mtime)
  local c = self.conn
  local path, rel, code = self:path(text)
  if not path then return c:send(err_line(rel, code)) end
  offset, size, mtime = int(offset), int(size), int(mtime)
  if not offset or not size or not mtime or offset < 0 then
    return c:send(err_line("bad GET request", "EINVAL"))
  end
  local st, serr, serrno = fs.stat(path)
  if not st then return c:send(err_line(serr, mft.code_of(serrno))) end
  if st.type == "directory" then return c:send(err_line("is a folder", "EISDIR")) end
  if st.size ~= size or st.mtime ~= mtime then return c:send(err_line("the file changed", "ESTALE")) end
  if offset > size then return c:send(err_line("offset beyond the end", "EINVAL")) end
  local f, ferr, ferrno = io.open(path, "rb")
  if not f then return c:send(err_line(ferr, mft.code_of(ferrno))) end
  f:seek("set", offset)
  local ok = c:send("OK\n")
  local done = offset
  while ok and done < size do
    if c:readable() then
      local line = c:line(STALL_MS)
      f:close()
      if line == "A" then
        return c:send("A\n")
      end
      return false
    end
    local data = f:read(math.min(mft.CHUNK, size - done))
    if not data or #data == 0 then
      f:close()
      return c:send("X EIO the file became shorter\n")
    end
    ok = c:send(chunk_header(data) .. data)
    done = done + #data
  end
  f:close()
  if not ok then return false end
  self:log(string.format("sent %s, %d bytes%s", rel or ".", size, offset > 0 and ", resumed at " .. offset or ""))
  return c:send("E\n")
end

-- Receives a file. Returns false when the connection failed.
function Session:put(text, size, mtime)
  local c = self.conn
  local path, rel, code = self:path(text)
  if not path then return c:send(err_line(rel, code)) end
  size, mtime = int(size), int(mtime)
  if not size or not mtime or size < 0 then return c:send(err_line("bad PUT request", "EINVAL")) end
  if path == self.root then return c:send(err_line("is a folder", "EISDIR")) end
  local dir, name = mft.dirname(path), mft.basename(path)
  local dst = fs.stat(dir)
  if not dst then return c:send(err_line("the folder does not exist", "ENOENT")) end
  if dst.type ~= "directory" then return c:send(err_line("not a folder", "ENOTDIR")) end
  local target = fs.stat(path)
  if target and target.type == "directory" then return c:send(err_line("is a folder", "EISDIR")) end
  local part, offset = prepare_part(dir, name, size, mtime)
  local f, ferr, ferrno = io.open(part, "ab")
  if not f then return c:send(err_line(ferr, mft.code_of(ferrno))) end
  if not c:send(string.format("OK %d\n", offset)) then
    f:close()
    return false
  end
  local done, bad, failure = offset, false, nil
  while true do
    local line = c:line(STALL_MS)
    if not line then
      f:close()
      return false
    end
    local len, crc = line:match("^C (%d+) (%x+)$")
    if len then
      len = int(len)
      if len < 1 or len > mft.CHUNK then
        f:close()
        return false
      end
      local data = c:read(len, STALL_MS)
      if not data then
        f:close()
        return false
      end
      if not bad and not failure then
        if sys.crc32(data) ~= tonumber(crc, 16) then
          bad = true
        elseif done + len > size then
          failure = { "more data than the size", "EINVAL" }
        else
          local ok, werr, werrno = f:write(data)
          if ok then
            done = done + len
          else
            failure = { werr, mft.code_of(werrno) }
          end
        end
      end
    elseif line == "E" then
      break
    elseif line == "A" then
      f:close()
      return c:send(err_line("the transfer was cancelled", "ECANCELED"))
    else
      f:close()
      return false
    end
  end
  local closed = f:close()
  if bad then return c:send(err_line("checksum mismatch", "EIO")) end
  if failure then return c:send(err_line(failure[1], failure[2])) end
  if not closed then return c:send(err_line("cannot write the file", "EIO")) end
  if done ~= size then return c:send(err_line("the data ended early", "EIO")) end
  fs.utime(part, mtime)
  local ok, rerr, rerrno = os.rename(part, path)
  if not ok then return c:send(err_line(rerr, mft.code_of(rerrno))) end
  self:log(string.format("stored %s, %d bytes%s", rel, size, offset > 0 and ", resumed at " .. offset or ""))
  return c:send("OK\n")
end

-- serve_session(fd, root [, opts]) answers the requests of one client
-- until QUIT, the end of the connection or opts.stopped(). opts.log(line)
-- receives one line per completed transfer. The function closes fd.
function mft.serve_session(fd, root, opts)
  opts = opts or {}
  local c = mft.wrap(fd)
  local self = setmetatable({ conn = c, root = root, opts = opts }, Session)
  local function next_line()
    while true do
      local line, msg, code = c:line(IDLE_MS)
      if line then return line end
      if code ~= "ETIMEDOUT" then return nil end
      if opts.stopped and opts.stopped() then return nil end
    end
  end
  local hello = next_line()
  if hello ~= "MFT 1" then
    if hello then c:send("ERR EPROTO unsupported version\n") end
    c:close()
    return
  end
  local label = mft.basename(root)
  if not c:send("MFT 1 " .. mft.encode(label) .. "\n") then
    c:close()
    return
  end
  while true do
    local line = next_line()
    if not line then break end
    local w = split(line)
    local cmd, ok = w[1], true
    if cmd == "LIST" then ok = c:send(self:list(w[2]))
    elseif cmd == "STAT" then ok = c:send(self:stat(w[2]))
    elseif cmd == "GET" then ok = self:get(w[2], w[3], w[4], w[5])
    elseif cmd == "PUT" then ok = self:put(w[2], w[3], w[4])
    elseif cmd == "MKDIR" then ok = c:send(self:mkdir(w[2]))
    elseif cmd == "RENAME" then ok = c:send(self:rename(w[2], w[3]))
    elseif cmd == "DELETE" then ok = c:send(self:delete(w[2]))
    elseif cmd == "QUIT" then
      c:send("OK\n")
      break
    elseif cmd == "A" then
      -- A late cancel of a GET that had ended. It has no reply.
    else
      ok = c:send(err_line("unknown request", "EINVAL"))
    end
    if not ok then break end
  end
  c:close()
end

-- serve(listen_fd, root, opts) accepts clients until opts.stopped()
-- returns true. Each client receives a worker thread that runs the
-- script opts.session (session.lua), which calls mft.serve_session.
-- opts.log(line) receives the lines of the sessions. opts.clients(n)
-- receives the number of connected clients after each change.
function mft.serve(lfd, root, opts)
  local thread = require "thread"
  local sessions = {}
  local function drain(s)
    while true do
      local msg = s.worker:receive(0)
      if not msg then break end
      call(opts.log, msg)
    end
  end
  local function reap(all)
    local changed = false
    for i = #sessions, 1, -1 do
      local s = sessions[i]
      drain(s)
      if all then s.worker:stop() end
      if all or s.worker:status() ~= "running" then
        s.worker:join()
        drain(s)
        s.worker:close()
        table.remove(sessions, i)
        changed = true
      end
    end
    if changed then call(opts.clients, #sessions) end
  end
  while not (opts.stopped and opts.stopped()) do
    local fd, address, port = net.accept(lfd, IDLE_MS)
    if fd then
      local worker, msg = thread.spawn(opts.session, fd .. "\n" .. root)
      if worker then
        sessions[#sessions + 1] = { worker = worker, address = address .. ":" .. port }
        call(opts.clients, #sessions)
      else
        sys.close(fd)
        call(opts.log, "cannot start a session: " .. tostring(msg))
      end
    end
    reap(false)
  end
  reap(true)
end

return mft
