-- This entire file runs in the audio thread's private Lua state. The GUI
-- sends copied commands and never touches this engine or its audio objects.
local thread, sys, audio = require 'thread', require 'sys', require 'audio'
local root = arg[0]:match('^(.*[/])') or './'
local Synth = dofile(root .. 'engine.lua')
local initial = ...
local engine = Synth.new(assert(Synth.decode(initial)))
local connection <close> = assert(audio.connect())
local playback <close> = assert(connection:playback('Lua Synthesizer'))
local quantum = playback:info().quantum
local tid = sys.thread_id()
local frames, render_ns, render_max_ns = 0, 0, 0
local cpu_start = sys.usage('thread').cpu_ns
local last_meter = 0
local pending_barrier
local function report()
  local scope = engine.scope
  local packet = 'M' .. string.pack('<I4I4fI8I8I8I8I8', engine:active(), playback:info().xruns,
    engine.peak, render_ns, render_max_ns, sys.usage('thread').cpu_ns - cpu_start, tid, frames)
  packet = packet .. string.pack('<' .. string.rep('f', #scope), table.unpack(scope))
  -- Meters are best effort. A stalled GUI must never stall sound generation.
  local ok = thread.send(packet)
  last_meter = sys.uptime()
  return ok
end
local function command(s)
  local op, rest = s:match('^(%S+)%s*(.*)$')
  if op == 'on' then
    local note, velocity = rest:match('^(%d+) (%d+)$')
    engine:note_on(assert(tonumber(note)), assert(tonumber(velocity)))
    thread.send(string.format('N%d %d', note, engine:active()))
  elseif op == 'off' then engine:note_off(assert(tonumber(rest)))
  elseif op == 'pedal' then engine:set_pedal(rest == '1')
  elseif op == 'all_off' then engine:all_off()
  elseif op == 'panic' then engine:panic()
  elseif op == 'set' then
    local key, value = rest:match('^(%S+) ([+-]?%d+)$')
    assert(engine:set(key, tonumber(value)))
  elseif op == 'patch' then assert(engine:apply(assert(Synth.decode(rest))))
  elseif op == 'barrier' then pending_barrier = rest
  else error('unknown audio command: ' .. tostring(op)) end
end
local function commands()
  -- Bound control work so a slider flood cannot indefinitely postpone PCM.
  for _ = 1, 32 do
    local s, err, code = thread.receive()
    if not s then
      if code ~= sys.errno.ETIMEDOUT and code ~= sys.errno.EPIPE then error(err) end
      return
    end
    command(s)
  end
end
local function pump()
  local count = playback:ready() // quantum
  for _ = 1, count do
    if thread.stop_requested() then return end
    local begin = sys.clock_ns()
    local pcm = engine:render(quantum)
    local elapsed = sys.clock_ns() - begin
    render_ns, render_max_ns = render_ns + elapsed, math.max(render_max_ns, elapsed)
    assert(playback:write(pcm) == quantum, 'incomplete audio write')
    frames = frames + quantum
  end
end
pump()
assert(playback:start())
assert(thread.send('R' .. tid))
local fds = {{fd=thread.fd(), events=sys.POLLIN}, {fd=connection:fd(), events=sys.POLLIN}}
while not thread.stop_requested() do
  local ok, err, code = sys.poll(fds, 20)
  if not ok and code ~= sys.errno.EINTR then error(err) end
  assert(connection:dispatch(0))
  commands()
  pump()
  if pending_barrier then
    if report() and thread.send('B' .. pending_barrier) then pending_barrier = nil end
  elseif sys.uptime() - last_meter >= 100 then report() end
end
report()
-- The to-be-closed playback and connection finish before thread.join returns.
