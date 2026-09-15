-- Deterministic failure injection, PCM conversion and lifetime checks.
local audio = require "audio"
local backend = audio_test
local checks = 0
local function check(ok, name)
  assert(ok, name)
  checks = checks + 1
end
local function rejects(fn, name)
  check(not pcall(fn), name)
end
local function empty()
  collectgarbage("collect")
  collectgarbage("collect")
  local c, p, r, m = backend.stats()
  check(c == 0 and p == 0 and r == 0 and m == 0, "all resources released")
end
local function fails(op, fn)
  backend.fail(op, 5)
  local value, message, code = fn()
  check(value == nil and type(message) == "string" and code == 5, op .. " errno result")
end
check(audio.rate == 48000 and audio.channels == 2 and audio.frame_bytes == 4 and audio.format == "s16le", "format")
fails("connect", audio.connect)
empty()
local c = assert(audio.connect())
check(c:fd() >= 0 and c:dispatch() == 0 and c:dispatch(-1) == 0 and c:sync(), "connection")
rejects(function() c:dispatch(-2) end, "negative timeout")
rejects(function() c:dispatch(math.maxinteger) end, "timeout overflow")
fails("dispatch", function() return c:dispatch() end)
fails("sync", function() return c:sync() end)
for _, kind in ipairs {"playback", "capture", "mixer"} do
  fails("create", function() return c[kind](c) end)
end
rejects(function() c:playback("a\0b") end, "NUL name")
rejects(function() c:playback(string.rep("a", 48)) end, "long name")
rejects(function() c:capture("bad", "unknown") end, "capture source")
local p = assert(c:playback("tone"))
local r = assert(c:capture("monitor", "monitor"))
local m = assert(c:mixer())
local fmt = p:info()
check(fmt.rate == 48000 and fmt.channels == 2 and fmt.quantum == 480 and fmt.state == "paused" and fmt.error == 0 and fmt.xruns == 0, "playback format")
check(r:info().rate == 48000 and p:ready() == 1440 and r:available() == 480, "ready counts are frames")
check(p:start() and r:start() and p:info().state == "running" and r:info().state == "running", "start")
check(p:volume(0) and p:volume(200) and r:volume(100), "stream volume")
for _, n in ipairs {-1, 201, math.maxinteger, 1.5} do
  rejects(function() p:volume(n) end, "bad playback volume")
  rejects(function() r:volume(n) end, "bad capture volume")
end
fails("control", function() return p:pause() end)
local frame = string.pack("<i2i2", -32768, 32767)
check(p:write(frame:rep(3)) == 3 and p:write("") == 0, "write frames")
local pcm, frames = r:read(3)
check(pcm == frame:rep(3) and frames == 3, "PCM roundtrip")
local blank, zero = r:read(0)
check(blank == "" and zero == 0, "zero capture")
check(#r:read() == 480 * 4, "default quantum")
backend.partial(1)
check(p:write(frame:rep(3)) == 1, "partial write")
backend.partial(2)
pcm, frames = r:read(3)
check(pcm == frame:rep(2) and frames == 2, "partial read")
rejects(function() p:write("bad") end, "incomplete PCM frame")
rejects(function() p:write(1234) end, "numeric PCM rejected")
rejects(function() p:write({}) end, "table PCM rejected")
for _, n in ipairs {-1, math.maxinteger, 1.5, math.huge} do
  rejects(function() r:read(n) end, "invalid capture size")
end
fails("write", function() return p:write(frame) end)
fails("read", function() return r:read(1) end)
local list = m:streams()
check(#list == 2 and list[1].direction == "capture" and list[2].name == "tone", "mixer snapshot")
local id = list[2].id
local generation = m:generation()
check(m:master() == 100 and m:master(75) and m:sync() and m:master() == 75, "master volume")
check(m:volume(id, 60) and m:sync() and m:generation() > generation, "mixer updates")
check(list[2].volume == 200 and m:streams()[2].volume == 60, "snapshot is independent")
rejects(function() m:volume(0, 100) end, "zero stream id")
rejects(function() m:volume(math.maxinteger, 100) end, "stream id overflow")
rejects(function() m:master(201) end, "bad master volume")
check(p:pause() and p:drain() and r:stop(), "stop and drain")
-- A child keeps its connection alive, even without a Lua connection variable.
local weak = setmetatable({c}, {__mode = "v"})
c = nil
collectgarbage("collect")
check(weak[1] ~= nil and p:info().rate == 48000, "child retains connection")
p:close(); p:close(); r:close(); m:close()
rejects(function() p:write(frame) end, "closed playback")
empty()
check(weak[1] == nil, "closing children releases owner")
-- Closing the owner first invalidates all its children without double frees.
c = assert(audio.connect())
p = assert(c:playback()); r = assert(c:capture()); m = assert(c:mixer())
c:close(); c:close()
for _, fn in ipairs {
  function() p:info() end, function() r:read() end, function() m:streams() end,
  function() c:fd() end, function() c:playback() end,
} do rejects(fn, "closed owner") end
p:close(); r:close(); m:close()
empty()
-- To-be-closed objects, including unwinding an error and reverse close order.
local saved
rejects(function()
  local scoped <close> = assert(audio.connect())
  local stream <close> = assert(scoped:playback())
  saved = stream
  error("unwind")
end, "scope unwinding")
rejects(function() saved:info() end, "unwound stream")
empty()
do
  local stream <close> = assert(audio.connect()):playback()
  stream:close()
end
empty()
for _ = 1, 20 do
  local owner = assert(audio.connect())
  owner:playback(); owner:capture(); owner:mixer()
end
empty()
arg = {"440", "0.02"}
dofile("user/share/lua/examples/tone.lua")
empty()
print("audio: " .. checks .. " host checks passed")
