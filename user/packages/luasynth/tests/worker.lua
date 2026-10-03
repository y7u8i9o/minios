local root = arg[1] or 'user/packages/luasynth/files/usr/share/apps/luasynth/'
local Synth = dofile(root .. 'engine.lua')
local Control = dofile(root .. 'controller.lua')
local thread, sys = require 'thread', require 'sys'
local c = Control.new(Synth)
assert(c:sync())
assert(c.ready and c.thread_id ~= sys.thread_id(), 'audio has a separate native thread')
assert(thread.active() == 1, 'exactly one worker')
for _, n in ipairs{48, 55, 60, 64, 67, 72, 76, 79} do assert(c:note_on(n)) end
assert(c:sync())
assert(c:active() == 8, 'worker applied eight-note chord')
local frames = c.stats.frames
-- Deliberately stop consuming GUI events. The worker must continue generating.
sys.sleep(250)
assert(c:sync())
assert(c.stats.frames > frames + 4800, 'audio progresses while GUI sleeps')
assert(c:set('cutoff', 1800))
assert(c:apply(Synth.presets[4]))
assert(c:sync() and c:active() == 0, 'patch atomically stops old voices')
assert(c:note_on(60))
assert(c:set_pedal(true))
assert(c:note_off(60))
assert(c:sync() and c:active() == 1, 'pedal controlled across states')
assert(c:panic())
assert(c:sync() and c:active() == 0, 'panic reaches worker')
assert(c:close())
c:close()
assert(thread.active() == 0, 'audio worker joined')
if audio_test then
  collectgarbage('collect')
  local connections, playback, capture, mixer = audio_test.stats()
  assert(connections == 0 and playback == 0 and capture == 0 and mixer == 0, 'worker audio cleaned up')
end
print('luasynth: native worker checks passed')
