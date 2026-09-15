-- Pure Lua synthesis engine. No GUI, audio service or executable patch files.
local M = {rate = 48000, max_voices = 8, waves = {"Sine", "Saw", "Pulse", "Triangle"}}
local sin, cos, pi, abs, min, max, floor = math.sin, math.cos, math.pi, math.abs, math.min, math.max, math.floor
local pack, concat = string.pack, table.concat
local unpack = table.unpack
local formats = {}
for n = 2, 128, 2 do formats[n] = "<" .. string.rep("i2", n) end
local sine = {}
for i = 0, 2047 do sine[i] = sin(2 * pi * i / 2048) end
-- key, label, minimum, maximum, default, display unit
M.controls = {
  {"wave_a", "Oscillator A", 1, 4, 2}, {"wave_b", "Oscillator B", 1, 4, 3},
  {"mix", "Oscillator blend", 0, 100, 40, "%"}, {"detune", "B detune", -50, 50, 7, " cents"},
  {"pulse", "Pulse width", 10, 90, 50, "%"}, {"octave_b", "B octave", -2, 2, 0, ""},
  {"attack", "Attack", 1, 1500, 12, " ms"}, {"decay", "Decay", 1, 1500, 180, " ms"},
  {"sustain", "Sustain", 0, 100, 65, "%"}, {"release", "Release", 5, 2500, 320, " ms"},
  {"cutoff", "Filter cutoff", 80, 12000, 2400, " Hz"}, {"resonance", "Resonance", 0, 90, 25, "%"},
  {"filter_env", "Envelope to filter", 0, 8000, 1600, " Hz"}, {"lfo_rate", "Vibrato rate", 1, 120, 45, " /10 Hz"},
  {"vibrato", "Vibrato depth", 0, 50, 0, " cents"}, {"spread", "Stereo spread", 0, 100, 65, "%"},
  {"delay_ms", "Delay time", 40, 700, 260, " ms"}, {"delay_mix", "Delay mix", 0, 60, 18, "%"},
  {"feedback", "Delay feedback", 0, 75, 32, "%"}, {"level", "Output level", 0, 100, 70, "%"},
}
local schema = {}
for _, c in ipairs(M.controls) do schema[c[1]] = c end
M.presets = {
  {name = "Warm pad", attack = 240, decay = 450, sustain = 75, release = 1000, wave_a = 4, wave_b = 2, detune = 11, cutoff = 1800, filter_env = 1800, vibrato = 5, delay_mix = 25, spread = 90},
  {name = "Bright keys", attack = 3, decay = 260, sustain = 30, release = 230, cutoff = 3800, filter_env = 5000, delay_mix = 12},
  {name = "Deep bass", wave_a = 2, wave_b = 3, octave_b = -1, detune = 0, attack = 4, decay = 200, sustain = 55, release = 110, cutoff = 650, resonance = 40, filter_env = 2200, spread = 0, delay_mix = 0},
  {name = "Glass", wave_a = 1, wave_b = 1, octave_b = 1, mix = 30, detune = 5, attack = 2, decay = 900, sustain = 15, release = 700, cutoff = 9000, filter_env = 0, delay_mix = 32, delay_ms = 340, feedback = 45},
  {name = "Pulse lead", wave_a = 3, wave_b = 2, pulse = 30, detune = 13, attack = 5, decay = 170, sustain = 75, release = 180, cutoff = 2300, resonance = 55, vibrato = 8, spread = 25},
  {name = "Pure sine", wave_a = 1, wave_b = 1, detune = 0, attack = 5, decay = 100, sustain = 100, release = 120, cutoff = 12000, resonance = 0, filter_env = 0, spread = 0, delay_mix = 0},
}
function M.patch(values)
  local p = {}
  for _, c in ipairs(M.controls) do
    local v = values and values[c[1]]
    if v == nil then v = c[5] end
    if type(v) ~= "number" or v ~= floor(v) or v < c[3] or v > c[4] then
      return nil, "Invalid " .. c[2]
    end
    p[c[1]] = v
  end
  return p
end
function M.encode(p)
  local valid, err = M.patch(p)
  if not valid then return nil, err end
  local lines = {"# LuaSynth patch 1"}
  for _, c in ipairs(M.controls) do lines[#lines + 1] = c[1] .. "=" .. valid[c[1]] end
  return concat(lines, "\n") .. "\n"
end
function M.decode(text)
  if type(text) ~= "string" or #text > 8192 then return nil, "Patch is too large" end
  local p, count = {}, 0
  for raw in (text .. "\n"):gmatch("([^\n]*)\n") do
    local line = raw:match("^%s*(.-)%s*$")
    if line ~= "" and line:sub(1, 1) ~= "#" then
      local key, number = line:match("^([%w_]+)%s*=%s*([+-]?%d+)$")
      if not key or not schema[key] or p[key] ~= nil then return nil, "Invalid patch line: " .. line end
      p[key], count = tonumber(number), count + 1
    end
  end
  if count == 0 then return nil, "Patch contains no sound controls" end
  return M.patch(p)
end
-- Polynomial correction at a discontinuity, in units of the current period.
local function blep(t, dt)
  if t < dt then t = t / dt; return t + t - t * t - 1 end
  if t > 1 - dt then t = (t - 1) / dt; return t * t + t + t + 1 end
  return 0
end
local function osc(wave, phase, dt, pulse)
  if wave == 1 then return sine[phase * 2048 // 1] end
  if wave == 2 then return 2 * phase - 1 - blep(phase, dt) end
  if wave == 3 then
    local t = phase - pulse
    if t < 0 then t = t + 1 end
    return (phase < pulse and 1 or -1) + blep(phase, dt) - blep(t, dt) - (2 * pulse - 1)
  end
  return 1 - 4 * abs(phase - 0.5)
end
-- Cache corrected waveforms so the sample loop only performs table lookups.
-- Round frequency upward to quarter-octave bands: the discontinuity is never
-- narrower than the actual phase step, including while vibrato changes pitch.
-- Bound this shared cache when editing pulse width across many notes.
local wave_cache, cache_count = {}, 0
local log2 = math.log(2)
local function waveform(wave, dt, pulse)
  if wave == 1 then return sine end
  local band = wave == 4 and 0 or math.ceil(math.log(dt) / log2 * 4)
  local key = wave .. ":" .. band .. ":" .. (wave == 3 and pulse or 0)
  local samples = wave_cache[key]
  if samples then return samples end
  if cache_count == 128 then wave_cache, cache_count = {}, 0 end
  samples = {}
  local step = min(0.5, 2 ^ (band / 4))
  for i = 0, 2047 do samples[i] = osc(wave, i / 2048, step, pulse) end
  wave_cache[key], cache_count = samples, cache_count + 1
  return samples
end
local Engine = {}; Engine.__index = Engine
function M.new(patch)
  return setmetatable({params = assert(M.patch(patch)), voices = {}, serial = 0, pedal = false,
    clock = 0, dl = {}, dr = {}, delay_pos = 1, z1l = 0, z2l = 0, z1r = 0, z2r = 0, scope = {}, peak = 0,
    left = {}, right = {}, pcm = {}, samples = {}}, Engine)
end
function Engine:set(key, value)
  local c = schema[key]
  if not c or type(value) ~= "number" or value ~= floor(value) or value < c[3] or value > c[4] then
    return nil, "Invalid sound control: " .. tostring(key)
  end
  self.params[key] = value
  return true
end
function Engine:apply(patch)
  local p, err = M.patch(patch)
  if not p then return nil, err end
  self:panic(); self.params = p
  return true
end
local function release(v, ms)
  v.stage, v.release_step = 4, v.env / max(1, ms * 48)
end
function Engine:note_on(note, velocity)
  assert(type(note) == "number" and note == floor(note) and note >= 24 and note <= 108, "Note out of range")
  velocity = velocity or 100
  assert(type(velocity) == "number" and velocity >= 1 and velocity <= 127, "Velocity out of range")
  for _, v in ipairs(self.voices) do
    if v.note == note and v.held then return end -- ignore keyboard auto-repeat
  end
  if #self.voices == M.max_voices then
    local index = 1
    for i = 2, #self.voices do
      local a, b = self.voices[i], self.voices[index]
      if (not a.held and b.held) or (a.held == b.held and (a.env < b.env or (a.env == b.env and a.serial < b.serial))) then index = i end
    end
    table.remove(self.voices, index)
  end
  self.serial = self.serial + 1
  self.voices[#self.voices + 1] = {note = note, velocity = velocity / 127, held = true, serial = self.serial,
    phase_a = 0, phase_b = 0.17, env = 0, stage = 1, frequency = 440 * 2 ^ ((note - 69) / 12)}
end
function Engine:note_off(note)
  for _, v in ipairs(self.voices) do
    if v.note == note and v.held then
      v.held = false
      if not self.pedal then release(v, self.params.release) end
    end
  end
end
function Engine:set_pedal(down)
  self.pedal = not not down
  if not self.pedal then
    for _, v in ipairs(self.voices) do
      if not v.held and v.stage ~= 4 then release(v, self.params.release) end
    end
  end
end
function Engine:all_off()
  self:set_pedal(false)
  for _, v in ipairs(self.voices) do
    if v.held then v.held = false; release(v, self.params.release) end
  end
end
function Engine:panic()
  self.voices, self.dl, self.dr, self.scope = {}, {}, {}, {}
  self.pedal, self.delay_pos = false, 1
  self.z1l, self.z2l, self.z1r, self.z2r, self.peak = 0, 0, 0, 0, 0
end
function Engine:active() return #self.voices end
function Engine:render(frames)
  assert(type(frames) == "number" and frames == floor(frames) and frames >= 0 and frames <= 4096, "Invalid render block")
  if frames == 0 then return "" end
  local p, left, right = self.params, self.left, self.right
  for i = 1, frames do left[i], right[i] = 0, 0 end
  local mix, pulse = p.mix / 100, p.pulse / 100
  local attack, decay, sustain = 1 / (p.attack * 48), (1 - p.sustain / 100) / (p.decay * 48), p.sustain / 100
  local vibrato = 2 ^ (sin(2 * pi * self.clock / M.rate * p.lfo_rate / 10) * p.vibrato / 1200)
  local ratio_b = 2 ^ (p.octave_b + p.detune / 1200)
  local max_env = 0
  for _, v in ipairs(self.voices) do
    local a, b, env, stage = v.phase_a, v.phase_b, v.env, v.stage
    local da = min(0.45, v.frequency / M.rate * vibrato)
    local db = min(0.45, da * ratio_b)
    local pan = ((v.note * 7) % 11 - 5) / 5 * p.spread / 100
    local gl, gr = (1 - pan) * 0.5 * v.velocity, (1 + pan) * 0.5 * v.velocity
    local wave_a, wave_b = waveform(p.wave_a, da, pulse), waveform(p.wave_b, db, pulse)
    local rs = v.release_step or 0
    for i = 1, frames do
      if stage == 1 then
        env = env + attack
        if env >= 1 then env, stage = 1, 2 end
      elseif stage == 2 then
        env = env - decay
        if env <= sustain then env, stage = sustain, 3 end
      elseif stage == 3 then env = sustain
      elseif stage == 4 then
        env = env - rs
        if env <= 0 then env, stage = 0, 0 end
      end
      local sample = (wave_a[a * 2048 // 1] * (1 - mix) + wave_b[b * 2048 // 1] * mix) * env
      left[i], right[i] = left[i] + sample * gl, right[i] + sample * gr
      a, b = a + da, b + db
      if a >= 1 then a = a - 1 end
      if b >= 1 then b = b - 1 end
    end
    v.phase_a, v.phase_b, v.env, v.stage = a, b, env, stage
    max_env = max(max_env, env)
  end
  for i = #self.voices, 1, -1 do if self.voices[i].stage == 0 then table.remove(self.voices, i) end end
  -- Shared resonant stereo low-pass, with envelope modulation per block.
  local cutoff = min(18000, p.cutoff + p.filter_env * max_env)
  local omega = 2 * pi * cutoff / M.rate
  local cs, alpha = cos(omega), sin(omega) / (2 * (0.5 + p.resonance / 15))
  local inv = 1 / (1 + alpha)
  local b0, b1, b2 = (1 - cs) * 0.5 * inv, (1 - cs) * inv, (1 - cs) * 0.5 * inv
  local a1, a2 = -2 * cs * inv, (1 - alpha) * inv
  local z1l, z2l, z1r, z2r = self.z1l, self.z2l, self.z1r, self.z2r
  local dl, dr, pos = self.dl, self.dr, self.delay_pos
  local delay, wet, feedback, gain = p.delay_ms * 48, p.delay_mix / 100, p.feedback / 100, p.level / 100 * 0.42
  if pos > delay then pos = 1 end
  local pcm, samples, scope, peak = self.pcm, self.samples, {}, 0
  local used, chunks = 0, 0
  local stride = max(1, frames // 128)
  for i = 1, frames do
    local l = b0 * left[i] + z1l
    z1l, z2l = b1 * left[i] - a1 * l + z2l, b2 * left[i] - a2 * l
    local r = b0 * right[i] + z1r
    z1r, z2r = b1 * right[i] - a1 * r + z2r, b2 * right[i] - a2 * r
    local el, er = dl[pos] or 0, dr[pos] or 0
    dl[pos], dr[pos] = l + er * feedback, r + el * feedback
    l, r = ((1 - wet) * l + wet * el) * gain, ((1 - wet) * r + wet * er) * gain
    -- Bounded saturation prevents integer wrap even with eight loud voices.
    l, r = l / (1 + abs(l)), r / (1 + abs(r))
    peak = max(peak, abs(l), abs(r))
    -- Convert 64 frames per C call instead of allocating 480 tiny strings
    -- per quantum. Scratch arrays are retained across render calls.
    samples[used + 1], samples[used + 2] = floor(l * 32767), floor(r * 32767)
    used = used + 2
    if used == 128 then
      chunks = chunks + 1
      pcm[chunks] = pack(formats[used], unpack(samples, 1, used))
      used = 0
    end
    if (i - 1) % stride == 0 then scope[#scope + 1] = l end
    pos = pos + 1
    if pos > delay then pos = 1 end
  end
  self.z1l, self.z2l, self.z1r, self.z2r = z1l, z2l, z1r, z2r
  self.delay_pos, self.clock, self.scope, self.peak = pos, self.clock + frames, scope, peak
  if used > 0 then
    chunks = chunks + 1
    pcm[chunks] = pack(formats[used], unpack(samples, 1, used))
  end
  for i = #pcm, chunks + 1, -1 do pcm[i] = nil end
  return concat(pcm)
end
return M
