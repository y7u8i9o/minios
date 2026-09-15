-- Real audiod playback, capture and mixer exercise (tests/cases/lua_audio).
local audio = require "audio"
if arg[1] == "no-server" then
  local c, message, code = audio.connect()
  assert(c == nil and type(message) == "string" and type(code) == "number" and code > 0)
  print("lua_audio: unavailable server reported")
  return
end
local c <close> = assert(audio.connect())
local p <close> = assert(c:playback("Lua tone"))
local monitor <close> = assert(c:capture("Lua monitor", "monitor"))
local input <close> = assert(c:capture("Lua input", "input"))
local mixer <close> = assert(c:mixer())
assert(c:fd() >= 0 and c:dispatch(0) >= 0)
assert(mixer:master(100) and p:volume(100) and monitor:volume(100))
local info = p:info()
local q = info.quantum
assert(info.rate == 48000 and info.channels == 2 and q > 0 and info.error == 0)
assert(monitor:info().rate == 48000 and input:info().channels == 2)
assert(p:ready() >= q)
local frame = string.pack("<i2i2", 12000, -6000)
local period = frame:rep(q)
for _ = 1, 3 do assert(p:write(period) == q) end
assert(monitor:start() and input:start() and p:start() and c:sync())
assert(p:info().state == "running" and monitor:info().state == "running")
local audible = 0
for _ = 1, 24 do
  local data, n = assert(monitor:read(q))
  assert(n == q and #data == q * 4)
  local heard = false
  for i = 1, #data, 4 do
    local left, right = string.unpack("<i2i2", data, i)
    assert((left == 0 and right == 0) or (left == 12000 and right == -6000), "PCM channels changed")
    heard = heard or left ~= 0
  end
  if heard then audible = audible + 1 end
  assert(p:write(period) == q)
  -- Half-buffer reads preserve the unconsumed half of captured input.
  for _ = 1, 2 do
    local data, n = assert(input:read(q // 2))
    assert(n == q // 2 and #data == n * 4 and data == string.rep("\0", #data))
  end
end
assert(audible >= 16, "monitor did not receive the Lua PCM signal")
print("lua_audio: monitor verified " .. audible .. " stereo periods")
assert(monitor:available() >= 0 and input:available() >= 0)
assert(monitor:stop() and input:stop() and p:drain() and p:pause() and c:sync())
assert(p:info().state == "paused" and monitor:info().state == "paused")
assert(mixer:sync())
local list = mixer:streams()
local id
for _, stream in ipairs(list) do
  if stream.name == "Lua tone" then
    assert(stream.direction == "playback" and stream.volume == 100 and stream.peak >= 0)
    id = stream.id
  end
end
assert(id and #list == 3)
local generation = mixer:generation()
assert(mixer:volume(id, 65) and mixer:master(80) and mixer:sync())
assert(mixer:master() == 80 and mixer:generation() > generation)
local changed
for _, stream in ipairs(mixer:streams()) do
  if stream.id == id then changed = stream.volume end
end
assert(changed == 65)
assert(mixer:master(100) and mixer:sync())
print("lua_audio: mixer verified")
-- Collecting a stream releases its native object and server registration.
local temporary = assert(c:playback("Lua temporary"))
assert(mixer:sync() and #mixer:streams() == 4)
temporary = nil
collectgarbage("collect")
assert(mixer:sync() and #mixer:streams() == 3)
-- A connection may be closed before any remaining userdata is finalized.
c:close()
assert(not pcall(function() p:start() end))
assert(not pcall(function() monitor:read(1) end))
assert(not pcall(function() mixer:streams() end))
p:close(); monitor:close(); input:close(); mixer:close()
print("lua_audio: lifetime verified")
print("lua_audio: done")
