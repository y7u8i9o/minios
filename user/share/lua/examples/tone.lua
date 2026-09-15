-- lua /usr/share/lua/examples/tone.lua [frequency_hz [seconds]]
-- A desktop session starts audiod; from a console, start it with audiod &.
local audio = require "audio"
local hz = tonumber(arg[1] or "440")
local seconds = tonumber(arg[2] or "0.5")
assert(hz and hz > 0 and hz < audio.rate / 2, "frequency must be between 0 and 24000 Hz")
assert(seconds and seconds > 0 and seconds <= 60, "duration must be between 0 and 60 seconds")
local connection <close> = assert(audio.connect())
local tone <close> = assert(connection:playback("Lua tone example"))
local quantum = tone:info().quantum
local total = math.floor(seconds * audio.rate)
assert(tone:start())
for first = 0, total - 1, quantum do
  local frames = math.min(quantum, total - first)
  local samples = {}
  for i = 0, frames - 1 do
    local sample = math.floor(6000 * math.sin(2 * math.pi * hz * (first + i) / audio.rate))
    samples[i + 1] = string.pack("<i2i2", sample, sample)
  end
  local pcm = table.concat(samples)
  while #pcm > 0 do
    local written = assert(tone:write(pcm))
    assert(written > 0, "audio write made no progress")
    pcm = pcm:sub(written * audio.frame_bytes + 1)
  end
end
assert(tone:drain())
