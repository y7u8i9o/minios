local root = arg[1] or "user/packages/luasynth/files/usr/share/apps/luasynth/"
local Synth = dofile(root .. "engine.lua")
local checks = 0
local function check(value, why) assert(value, why); checks = checks + 1 end
local function render(e, n)
  local blocks = {}
  while n > 0 do local q = math.min(n, 480); blocks[#blocks + 1] = e:render(q); n = n - q end
  return table.concat(blocks)
end
local function peak(pcm)
  local p = 0
  for i = 1, #pcm, 2 do p = math.max(p, math.abs(string.unpack("<i2", pcm, i))) end
  return p
end
local e = Synth.new(Synth.presets[6])
check(e:render(480) == string.rep("\0", 1920), "idle silence")
check(e:render(0) == "", "empty block")
e:note_on(69)
local pcm = render(e, 48000)
check(#pcm == 48000 * 4 and peak(pcm) > 1000, "nonzero stereo PCM")
local crosses, previous, centered = 0, 0, true
for i = 1, #pcm, 4 do
  local l, r = string.unpack("<i2i2", pcm, i)
  centered = centered and l == r
  if previous < 0 and l >= 0 then crosses = crosses + 1 end
  previous = l
end
check(centered, "centered sine has matching channels")
check(math.abs(crosses - 440) <= 2, "A4 frequency")
-- Changing the block size must neither retain old packed frames nor drop
-- a partial packing group. The sine patch has block-independent coefficients.
local variable, reference = Synth.new(Synth.presets[6]), Synth.new(Synth.presets[6])
variable:note_on(69); reference:note_on(69)
for _, n in ipairs {1, 63, 64, 65, 480, 4096, 7, 960, 1} do
  local block = variable:render(n)
  check(#block == n * 4 and block == render(reference, n), "variable PCM block size " .. n)
end
e:note_on(69); check(e:active() == 1, "auto-repeat suppressed")
e:note_off(69); render(e, 6000); check(e:active() == 0, "release ends")
e:panic(); check(e:render(480) == string.rep("\0", 1920), "panic clears filter and delay")
e:note_on(60); render(e, 1000); e:set_pedal(true); e:note_off(60); render(e, 10000)
check(e:active() == 1, "sustain retains released note")
e:set_pedal(false); render(e, 6000); check(e:active() == 0, "pedal release releases voice")
for n = 60, 71 do e:note_on(n) end
check(e:active() == 8, "voice limit")
e:all_off(); render(e, 6000); check(e:active() == 0, "all notes off")
local loud, quiet = Synth.new(Synth.presets[6]), Synth.new(Synth.presets[6])
loud:note_on(60, 120); quiet:note_on(60, 20)
check(peak(render(loud, 4800)) > peak(render(quiet, 4800)) * 3, "velocity affects amplitude")
for _, preset in ipairs(Synth.presets) do
  local decoded = assert(Synth.decode(assert(Synth.encode(assert(Synth.patch(preset))))))
  for _, spec in ipairs(Synth.controls) do check(decoded[spec[1]] == (preset[spec[1]] or spec[5]), "preset roundtrip") end
  local voice = Synth.new(preset)
  for _, n in ipairs {48, 55, 60, 64, 67, 72, 76, 79} do voice:note_on(n) end
  local pcm = render(voice, 4800)
  check(peak(pcm) > 0 and peak(pcm) <= 32767, "eight-voice preset output bounded")
end
for _, patch in ipairs {"", "level=101", "level=nan", "level=0/0", "level=10\nlevel=20", "unknown=3", "os.execute('bad')", "wave_a=7", "level=" .. string.rep("9", 9000)} do
  check(Synth.decode(patch) == nil, "invalid patch refused")
end
local before = e.params.cutoff
check(not e:apply {cutoff = -1} and e.params.cutoff == before, "invalid patch leaves state unchanged")
local stereo = Synth.new(Synth.presets[2]); stereo:note_on(60)
pcm = render(stereo, 2000)
local different = false
for i = 1, #pcm, 4 do local l, r = string.unpack("<i2i2", pcm, i); different = different or l ~= r end
check(different, "stereo spread")
local dry = Synth.new {wave_a = 1, wave_b = 1, detune = 0, spread = 0, attack = 1, release = 5, delay_mix = 0}
local echo = Synth.new {wave_a = 1, wave_b = 1, detune = 0, spread = 0, attack = 1, release = 5, delay_mix = 50, delay_ms = 100}
for _, v in ipairs {dry, echo} do v:note_on(60); render(v, 960); v:note_off(60); render(v, 3840) end
check(peak(render(echo, 1920)) > peak(render(dry, 1920)) + 100, "delay produces a tail")
local fast = Synth.new(Synth.presets[2]); for _, n in ipairs {48, 55, 60, 64, 67, 72, 76, 79} do fast:note_on(n) end
local start = os.clock(); render(fast, 4800)
-- MiniOS clock() measures elapsed time; the host implementation measures CPU.
print(string.format("luasynth: eight-voice render 100 ms in %.1f ms clock time", (os.clock() - start) * 1000))
print("luasynth: " .. checks .. " engine checks passed")
