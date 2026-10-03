local thread, sys = require 'thread', require 'sys'
local mode = ...
assert(_G.parent_only == nil, 'Lua globals leaked into worker')
if mode == 'error' then error('intentional worker error') end
if mode == 'exit' then os.exit(7) end
if mode == 'park' then
  assert(thread.send('parked')); sys.sleep(500)
  return
end
assert(not pcall(require, 'gui'), 'worker must not initialize process-global GUI')
assert(thread.send(string.format('%d %d', sys.pid(), sys.thread_id())))
if mode == 'gc' then thread.receive(-1); return end
while not thread.stop_requested() do
  local s, err, code = thread.receive(100)
  if s then
    if s == 'quit' then break end
    assert(thread.send(s))
  elseif code ~= sys.errno.ETIMEDOUT and code ~= sys.errno.EPIPE then error(err) end
end
