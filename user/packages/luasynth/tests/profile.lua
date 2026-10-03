-- The same instrument and GUI, with inline DSP or a native audio worker.
-- CPU values use sys.usage, elapsed render times use sys.clock_ns.
if arg[2] == 'worker' then
  dofile('/etc/tests/luasynth-worker.lua')
  print('AUDIOPROFILE complete')
  return
end
local root = arg[1] or '/usr/share/apps/luasynth/'
local Synth = dofile(root .. 'engine.lua')
local UI = dofile(root .. 'ui.lua')
local audio, sys, thread = require 'audio', require 'sys', require 'thread'
local duration = arg[2] == 'stress' and 10000 or 4000
local notes = {48, 55, 60, 64, 67, 72, 76, 79}
local function copy(t)
  local result = {}
  for k, v in pairs(t) do result[k] = v end
  return result
end
local function run(mode, voices)
  local inline = mode == 'inline'
  local engine = inline and Synth.new(Synth.presets[2]) or nil
  local view = UI.new(Synth, engine)
  engine = view.engine
  local connection, playback, watch
  local stats = {frames=0, render_ns=0, render_max_ns=0}
  if inline then
    connection = assert(audio.connect())
    playback = assert(connection:playback('Lua inline comparison'))
    local quantum = playback:info().quantum
    local function pump()
      for _ = 1, playback:ready() // quantum do
        local begin = sys.clock_ns()
        local pcm = engine:render(quantum)
        local elapsed = sys.clock_ns() - begin
        stats.render_ns = stats.render_ns + elapsed
        stats.render_max_ns = math.max(stats.render_max_ns, elapsed)
        stats.frames = stats.frames + quantum
        assert(playback:write(pcm) == quantum)
      end
      engine.xruns = playback:info().xruns
    end
    pump()
    assert(playback:start())
    watch = view.app:watch(connection:fd(), 'r', function()
      assert(connection:dispatch(0))
      pump()
    end)
  else
    assert(engine:sync())
    assert(engine.thread_id ~= sys.thread_id() and thread.active() == 1, 'two native threads')
  end
  -- Performance output should not be affected by console printing per note.
  engine.on_note = nil
  for i = 1, voices do engine:note_on(notes[i]) end
  local warm = sys.uptime() + 500
  while sys.uptime() < warm do assert(view.app:step(20)) end
  local function snapshot()
    if inline then
      assert(connection:dispatch(0))
      local s = copy(stats)
      s.cpu_ns = sys.usage('thread').cpu_ns
      s.xruns = playback:info().xruns
      return s
    end
    assert(engine:sync())
    local s = copy(engine.stats)
    s.xruns = engine.xruns
    return s
  end
  local first = snapshot()
  local begin, main_cpu = sys.clock_ns(), sys.usage('thread').cpu_ns
  local froze = false
  while (sys.clock_ns() - begin) / 1000000 < duration do
    if mode == 'blocked-gui' and not froze and (sys.clock_ns() - begin) >= 1000000000 then
      local before = snapshot()
      -- No GUI event processing at all during this interval.
      sys.sleep(500)
      local after = snapshot()
      assert(after.frames - before.frames >= 12000, 'audio stopped with GUI')
      print(string.format('THREADBLOCK frames=%d xruns=%d', after.frames-before.frames, after.xruns-before.xruns))
      froze = true
    end
    assert(view.app:step(20))
  end
  local last = snapshot()
  local elapsed = (sys.clock_ns() - begin) / 1000000
  local render = (last.render_ns-first.render_ns) / 1000000
  local frames = last.frames-first.frames
  print(string.format('THREADPROFILE mode=%s voices=%d wall_ms=%.1f frames=%d xruns=%d render_mean_ms=%.3f render_max_ms=%.3f audio_cpu_ms=%.1f gui_cpu_ms=%.1f',
    mode, voices, elapsed, frames, last.xruns-first.xruns, render/(frames/480),
    last.render_max_ns/1000000, (last.cpu_ns-first.cpu_ns)/1000000,
    (sys.usage('thread').cpu_ns-main_cpu)/1000000))
  if watch then watch:remove() end
  if playback then playback:close() end
  if connection then connection:close() end
  view:close()
  assert(thread.active() == 0, 'worker leaked after window close')
  sys.sleep(200)
end
local cases = {{'inline', 8}, {'threaded', 0}, {'threaded', 1}, {'threaded', 8}, {'blocked-gui', 8}}
for _, case in ipairs(cases) do run(case[1], case[2]) end
print('AUDIOPROFILE complete')
