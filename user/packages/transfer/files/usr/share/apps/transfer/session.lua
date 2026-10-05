-- A worker thread of the server of Transfer: one client session. The
-- initial data is the descriptor of the connection, a LF and the served
-- folder. The lines of the session go to the parent, which waits for no
-- reply. A stop request of the parent ends the session.
local thread = require "thread"
local mft = require "mft"

local fd, root = (...):match("^(%d+)\n(.*)$")
mft.serve_session(math.tointeger(tonumber(fd)), root, {
  log = function(line) thread.send(line, -1) end,
  stopped = thread.stop_requested,
})
