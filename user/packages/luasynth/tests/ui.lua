local root = "user/packages/luasynth/files/share/apps/luasynth/"
local Synth, UI = dofile(root .. "engine.lua"), dofile(root .. "ui.lua")
local gui = require "gui"
local v = UI.new(Synth, Synth.new(Synth.presets[2]))
gui.test.paint(v.win)
local w, h = v.win:size()
local x, y = v.keys:pos()
local kw, kh = v.keys:size()
local _, sy = v.status:pos(); local _, sh = v.status:size()
assert(sy + sh <= h, "status fits the window")
assert(x >= 0 and y >= 0 and x + kw <= w and y + kh <= h, "keyboard fits the window")
local function key(code, up) gui.test.key(v.win, code, 0, 0, up) end
key(0x2c); key(0x2e); key(0x30)
assert(v.engine:active() == 3, "three simultaneous keyboard notes")
key(0x2c); assert(v.engine:active() == 3, "repeat does not retrigger")
key(0x39); key(0x2c, true); key(0x2e, true); key(0x30, true)
assert(v.engine.pedal, "space sustain")
key(0x39, true); assert(not v.engine.pedal, "pedal key release")
key(0x01); assert(v.engine:active() == 0, "panic")
-- Mouse press, captured motion, and release carry coordinates through Lua.
gui.test.mouse(v.win, "down", x + 10, y + kh - 10, 1)
assert(v.counts[60] == 1, "mouse note on")
gui.test.mouse(v.win, "move", x + math.floor(kw / 15) + 10, y + kh - 10, 1)
assert(not v.counts[60] and v.counts[62] == 1, "glissando")
gui.test.mouse(v.win, "up", x + kw + 10, y + kh + 10, 0)
assert(not next(v.held), "release outside canvas")
v:panic()
key(0x2c)
gui.test.mouse(v.win, "down", x + 10, y + kh - 10, 1)
gui.test.mouse(v.win, "up", x + 10, y + kh - 10, 0)
assert(v.counts[60] == 1, "mouse release preserves keyboard-held note")
key(0x2c, true)
v:apply(Synth.presets[4], "Glass")
assert(v.controls.wave_a:value() == 0 and v.engine.params.delay_ms == 340, "preset controls")
v:apply(Synth.presets[2], "Bright keys")
assert(v.engine:set("detune", -12))
assert(v:save("build/luasynth-test.lsynth"))
assert(v.engine:set("detune", 0))
assert(v:load("build/luasynth-test.lsynth") and v.engine.params.detune == -12, "patch save/load")
os.remove("build/luasynth-test.lsynth")
v:press("preview1", 60); v:press("preview2", 64); v:press("preview3", 67)
v.engine:render(480); v:update_status(); gui.test.paint(v.win)
local out = os.getenv("LUASYNTH_PREVIEW")
if out then
  local f = assert(io.open(out, "wb")); f:write(string.format("P6\n%d %d\n255\n", w, h))
  for yy = 0, h - 1 do
    local row = {}
    for xx = 0, w - 1 do
      local rgb = gui.test.pixel(v.win, xx, yy)
      row[#row + 1] = string.char((rgb >> 16) & 255, (rgb >> 8) & 255, rgb & 255)
    end
    f:write(table.concat(row))
  end
  f:close()
end
-- Meter changes must not repaint the keyboard or relayout the whole window.
local keyboard_paints = 0
v.keys:on("paint", function() keyboard_paints = keyboard_paints + 1 end)
gui.test.paint(v.win); keyboard_paints = 0
v.message = "Status-only repaint"; v:update_status(); gui.test.paint(v.win)
assert(keyboard_paints == 0, "status change must not repaint the whole window")
v:close(); v:close()
collectgarbage("collect"); collectgarbage("collect")
local c, p, r, m = audio_test.stats()
assert(c == 0 and p == 0 and r == 0 and m == 0, "audio resources closed")
print("luasynth: GUI checks passed")
