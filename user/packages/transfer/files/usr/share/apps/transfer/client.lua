-- The worker thread of the client of the Transfer window. The worker owns
-- one connection and performs the requests of the window in order, as the
-- worker thread of tools/transfer.py does.
--
-- Messages are fields separated by NUL bytes. The window sends:
--   connect HOST PORT | disconnect | list PATH | mkdir PATH
--   rename FROM TO | delete PATH... | cancel
--   upload REMOTE_DIR LOCAL_PATH...
--   download LOCAL_DIR ENTRY...    (ENTRY is "KIND SIZE MTIME PATH", PATH percent encoded)
--   run ID | discard ID
-- The worker answers:
--   connected HOST PORT NAME | disconnected | failed WHAT MESSAGE CODE
--   lost MESSAGE                       (the connection broke)
--   listing PATH LINES                 (LINES as in a LIST reply)
--   done WHAT                          (mkdir, rename or delete)
--   planned ID CONFLICTS               (CONFLICTS separated by LF)
--   step INDEX DIRECTION KIND SIZE LABEL
--   start INDEX | progress INDEX DONE SIZE | finished INDEX MOVED
--   stepfailed INDEX STATUS | batch STATE  (STATE "done" or "cancelled")
-- A request that arrives during a transfer waits until the transfer ends,
-- except cancel.
local thread = require "thread"
local sys = require "sys"
local fs = require "fs"
local mft = require "mft"

local SEP = "\0"
local PROGRESS_NS = 100000000

local client
local plans, next_plan = {}, 1
local pending = {}
local cancel = false

local function post(...)
  thread.send(table.concat({ ... }, SEP), -1)
end

local function split(msg)
  local fields = {}
  for f in (msg .. SEP):gmatch("(.-)\0") do fields[#fields + 1] = f end
  return fields
end

-- Codes of a broken connection. The client is closed after one of them.
local broken = { ECONNRESET = true, EPIPE = true, ETIMEDOUT = true, EPROTO = true }

local function check_lost(code, msg)
  if client and broken[code] then
    client:close()
    client = nil
    post("lost", msg)
    return true
  end
  return false
end

local function fail(what, msg, code)
  if not check_lost(code, msg) then post("failed", what, msg or "error", code or "EIO") end
end

local function require_client(what)
  if client then return true end
  post("failed", what, "not connected", "ENOTCONN")
  return false
end

-- The worker reads its queue during a transfer: cancel takes effect at
-- once, and every other request waits in pending.
local function poll_queue()
  while true do
    local msg = thread.receive(0)
    if not msg then break end
    if msg == "cancel" then cancel = true else pending[#pending + 1] = msg end
  end
  return cancel or thread.stop_requested()
end

local function do_list(path)
  if not require_client("list") then return end
  local entries, msg, code = client:list(path)
  if not entries then return fail("list", msg, code) end
  local lines = {}
  for _, e in ipairs(entries) do
    lines[#lines + 1] = string.format("%s %d %d %s\n", e.kind, e.size, e.mtime, mft.encode(e.name))
  end
  -- A listing longer than one message goes out in parts. Each part ends
  -- with whole lines; the window joins them until the part without the
  -- marker "+".
  local text, start = table.concat(lines), 1
  while #text - start + 1 > 7000 do
    local stop = text:find("\n", start + 6000, true) or #text
    post("listing", path, "+", text:sub(start, stop))
    start = stop + 1
  end
  post("listing", path, "", text:sub(start))
end

local function conflicts_of(names, existing)
  local out = {}
  for _, n in ipairs(names) do
    if existing[n] then out[#out + 1] = n end
  end
  table.sort(out)
  return table.concat(out, "\n")
end

local function plan_upload(fields)
  if not require_client("upload") then return end
  local target = fields[2]
  local entries, msg, code = client:list(target)
  if not entries then return fail("upload", msg, code) end
  local existing = {}
  for _, e in ipairs(entries) do existing[e.name] = true end
  local plan, names = {}, {}
  for i = 3, #fields do
    local steps
    steps, msg, code = mft.upload_plan(fields[i], target)
    if not steps then return fail("upload", msg, code) end
    for _, s in ipairs(steps) do
      s.direction = "put"
      s.label = s.dst
      plan[#plan + 1] = s
    end
    names[#names + 1] = mft.basename(fields[i])
  end
  plans[next_plan] = plan
  post("planned", tostring(next_plan), conflicts_of(names, existing))
  next_plan = next_plan + 1
end

local function plan_download(fields)
  if not require_client("download") then return end
  local target = fields[2]
  local plan, names, existing = {}, {}, {}
  for i = 3, #fields do
    local kind, size, mtime, path = fields[i]:match("^([df]) (%d+) (%-?%d+) (%S+)$")
    path = path and mft.decode(path)
    if not path then return post("failed", "download", "bad entry", "EINVAL") end
    local steps, msg, code = mft.download_plan(client, path, kind, tonumber(size), tonumber(mtime), target)
    if not steps then return fail("download", msg, code) end
    for _, s in ipairs(steps) do
      s.direction = "get"
      s.label = s.src
      plan[#plan + 1] = s
    end
    local name = mft.basename(path)
    names[#names + 1] = name
    if fs.lstat(mft.local_join(target, name)) then existing[name] = true end
  end
  plans[next_plan] = plan
  post("planned", tostring(next_plan), conflicts_of(names, existing))
  next_plan = next_plan + 1
end

local function run_plan(id)
  local plan = plans[id]
  plans[id] = nil
  if not plan then return end
  cancel = false
  for i, s in ipairs(plan) do
    post("step", tostring(i), s.direction, s.kind, tostring(s.size), s.label)
  end
  for i, s in ipairs(plan) do
    if poll_queue() then
      for k = i, #plan do post("stepfailed", tostring(k), "Cancelled") end
      break
    end
    if not client then
      for k = i, #plan do post("stepfailed", tostring(k), "Not started") end
      break
    end
    post("start", tostring(i))
    local last = 0
    local moved, msg, code = mft.run_step(client, s.direction, s, {
      progress = function(done, size)
        local now = sys.clock_ns()
        if now - last >= PROGRESS_NS or done == size then
          last = now
          post("progress", tostring(i), tostring(done), tostring(size))
        end
      end,
      cancelled = poll_queue,
    })
    if moved then
      post("finished", tostring(i), tostring(moved))
    elseif code == "ECANCELED" then
      post("stepfailed", tostring(i), "Cancelled")
    else
      post("stepfailed", tostring(i), "Failed: " .. tostring(msg))
      check_lost(code, msg)
    end
  end
  post("batch", cancel and "cancelled" or "done")
  cancel = false
end

local function handle(msg)
  local f = split(msg)
  local cmd = f[1]
  if cmd == "connect" then
    if client then client:quit() end
    local c, err, code = mft.connect(f[2], math.tointeger(tonumber(f[3])) or 0)
    if not c then return post("failed", "connect", err, code) end
    client = c
    post("connected", f[2], f[3], c.name)
  elseif cmd == "disconnect" then
    if client then client:quit() end
    client = nil
    post("disconnected")
  elseif cmd == "list" then
    do_list(f[2])
  elseif cmd == "mkdir" then
    if not require_client("mkdir") then return end
    local ok, err, code = client:mkdir(f[2])
    if not ok then return fail("mkdir", err, code) end
    post("done", "mkdir")
  elseif cmd == "rename" then
    if not require_client("rename") then return end
    local ok, err, code = client:rename(f[2], f[3])
    if not ok then return fail("rename", err, code) end
    post("done", "rename")
  elseif cmd == "delete" then
    if not require_client("delete") then return end
    for i = 2, #f do
      local ok, err, code = client:delete(f[i])
      if not ok then return fail("delete", err, code) end
    end
    post("done", "delete")
  elseif cmd == "upload" then
    plan_upload(f)
  elseif cmd == "download" then
    plan_download(f)
  elseif cmd == "run" then
    run_plan(math.tointeger(tonumber(f[2])))
  elseif cmd == "discard" then
    plans[math.tointeger(tonumber(f[2])) or 0] = nil
  elseif cmd == "cancel" then
    cancel = false
  end
end

while not thread.stop_requested() do
  local msg
  if #pending > 0 then
    msg = table.remove(pending, 1)
  else
    local err, code
    msg, err, code = thread.receive(250)
    if not msg and code ~= sys.errno.ETIMEDOUT then break end
  end
  if msg then handle(msg) end
end
if client then client:quit() end
