-- The worker thread of the server of the Transfer window. The initial data
-- is the port, a LF and the served folder. The worker sends the messages
-- "listening PORT" or "failed MESSAGE" first, then "log LINE" for each
-- transfer and "clients N" for each change of the number of clients. A
-- stop request of the window ends the server and its sessions.
local thread = require "thread"
local net = require "net"
local sys = require "sys"
local mft = require "mft"

local port, root = (...):match("^(%d+)\n(.*)$")
local session = (arg[0]:match("^(.*/)") or "./") .. "session.lua"
local fs = require "fs"
local st = fs.stat(root)
if not st or st.type ~= "directory" then
  thread.send("failed " .. root .. " is not a folder", -1)
  return
end
local lfd, bound = net.listen(math.tointeger(tonumber(port)))
if not lfd then
  thread.send("failed " .. tostring(bound), -1)
  return
end
thread.send("listening " .. bound, -1)
mft.serve(lfd, root, {
  session = session,
  log = function(line) thread.send("log " .. line, -1) end,
  clients = function(n) thread.send("clients " .. n, -1) end,
  stopped = thread.stop_requested,
})
sys.close(lfd)
