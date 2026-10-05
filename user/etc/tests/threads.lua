local thread, sys = require 'thread', require 'sys'
local root = (arg[0]:match('^(.*[/])') or './')
local path = root .. 'thread-worker.lua'
local checks = 0
local function check(v, why) assert(v, why); checks = checks + 1 end
_G.parent_only = true
local w = assert(thread.spawn(path, 'echo'))
local hello = assert(w:receive(2000))
local pid, tid = hello:match('(%d+) (%d+)')
check(tonumber(pid) == sys.pid() and tonumber(tid) ~= sys.thread_id(), 'native thread in same process')
check(w:status() == 'running', 'worker status')
local binary = 'abc\0def' .. string.rep('z', 4000)
check(w:send(binary), 'send copied bytes')
local fds = {{fd=w:fd(), events=sys.POLLIN}}
check(sys.poll(fds, 2000) == 1 and fds[1].revents & sys.POLLIN ~= 0, 'pollable messages')
check(w:receive() == binary, 'binary round trip')
local value, _, code = w:receive(5)
check(value == nil and code == sys.errno.ETIMEDOUT, 'receive timeout')
check(w:send('quit') and w:join() and w:join(), 'repeat join')
check(w:status() == 'done' and sys.poll(fds, 0) == 1, 'completion remains pollable')
value, _, code = w:receive()
check(value == nil and code == sys.errno.EPIPE, 'closed channel')
check(w:close() and w:close(), 'repeat close')
check(not pcall(w.send, w, 'x'), 'closed handle rejected')
for _, mode in ipairs{'error', 'exit'} do
  local child = assert(thread.spawn(path, mode))
  local ok, err = child:join()
  check(ok == nil and type(err) == 'string' and #err > 0 and child:status() == 'error', 'worker errors contained')
  child:close()
end
local missing = assert(thread.spawn(root .. 'missing-worker.lua'))
check(not missing:join(), 'load failure contained'); missing:close()
local parker = assert(thread.spawn(path, 'park')); check(parker:receive(2000) == 'parked', 'queue test ready')
for _ = 1, 64 do assert(parker:send(string.rep('q', 1024))) end
value, _, code = parker:send('x')
check(not value and code == sys.errno.EAGAIN, 'bounded queue backpressure')
check(not pcall(parker.send, parker, string.rep('x', 8193)), 'oversize message rejected')
local started = sys.clock_ns()
value, _, code = parker:send('x', 50)
check(not value and code == sys.errno.EAGAIN and sys.clock_ns() - started >= 45000000, 'waiting send times out')
parker:close()
local flood = assert(thread.spawn(path, 'flood'))
local received = 0
while true do
  local m = flood:receive(2000)
  if not m then break end
  received = received + 1
  check(m:sub(1, 4) == string.format('%04d', received), 'waiting send preserves the order')
end
check(received == 300 and flood:join(), 'waiting send delivers every message')
flood:close()
local stuck = assert(thread.spawn(path, 'stuck'))
sys.sleep(100)
check(stuck:status() == 'running', 'the worker waits for queue space')
stuck:stop()
check(stuck:join(), 'a stop ends a waiting send in the worker')
stuck:close()
local waiting = assert(thread.spawn(path, 'gc')); assert(waiting:receive(2000))
waiting = nil; collectgarbage('collect')
local deadline = sys.uptime() + 2000
while thread.active() ~= 0 and sys.uptime() < deadline do sys.sleep(5) end
check(thread.active() == 0, 'GC stops blocked receiver and reclaims worker')
-- Separate endpoints under contention; no shared Lua object is touched.
local workers = {}
for i = 1, 3 do workers[i] = assert(thread.spawn(path)); assert(workers[i]:receive(2000)) end
for round = 1, 40 do
  for i, child in ipairs(workers) do assert(child:send(i .. ':' .. round)) end
  for i, child in ipairs(workers) do check(child:receive(2000) == i .. ':' .. round, 'concurrent message ordering') end
end
for _, child in ipairs(workers) do child:stop(); assert(child:join()); child:close() end
check(thread.active() == 0, 'all joined workers gone')
local r, out = assert(sys.pipe(true))
check(sys.nonblock(r) and sys.nonblock(out), 'nonblocking pipe')
check(sys.write(out, 'hello\0') == 6, 'descriptor write')
fds = {{fd=r, events=sys.POLLIN}}
check(sys.poll(fds, 0) == 1 and sys.read(r) == 'hello\0', 'descriptor poll and read')
sys.close(out); check(sys.poll(fds, 0) == 1 and sys.read(r) == nil, 'pipe EOF'); sys.close(r)
local wall, cpu = sys.clock_ns(), sys.usage('thread').cpu_ns
sys.sleep(30)
check(sys.clock_ns() - wall >= 20000000, 'nanosecond monotonic clock')
check(sys.usage('thread').cpu_ns - cpu < 20000000, 'thread CPU clock excludes sleep')
check(sys.usage('process').cpu_ns >= sys.usage('thread').cpu_ns, 'process and thread usage')
check(not pcall(sys.sleep, -1) and not pcall(sys.poll, {}, -2), 'invalid durations rejected')
print('threads: '..checks..' checks passed')
