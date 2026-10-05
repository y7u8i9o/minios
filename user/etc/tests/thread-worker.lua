local thread, sys = require 'thread', require 'sys'
local mode = ...
assert(_G.parent_only == nil, 'Lua globals leaked into worker')
if mode == 'error' then error('intentional worker error') end
if mode == 'exit' then os.exit(7) end
if mode == 'park' then
  assert(thread.send('parked')); sys.sleep(500)
  return
end
-- flood sends 300 messages of 1 KiB with a waiting send. The queue takes
-- 64 of them, so the worker waits until the parent receives.
if mode == 'flood' then
  for i = 1, 300 do assert(thread.send(string.format('%04d', i) .. string.rep('f', 1020), -1)) end
  return
end
-- stuck fills the queue and then waits without limit until a stop.
if mode == 'stuck' then
  for _ = 1, 64 do assert(thread.send(string.rep('s', 1024))) end
  local ok, _, code = thread.send('x', -1)
  assert(not ok and code == sys.errno.EAGAIN and thread.stop_requested(), 'a stop ends a waiting send')
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
