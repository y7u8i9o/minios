-- GUI thread: native widgets and messages to the independent audio worker.
local gui, sys = require "gui", require "sys"
local directory = debug.getinfo(1, "S").source:sub(2):match("^(.*[/])") or "./"
local Controller = dofile(directory .. "controller.lua")
local M = {}
local names = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"}
local white = {0, 2, 4, 5, 7, 9, 11, 12, 14, 16, 17, 19, 21, 23, 24}
local black = {[1] = 1, [3] = 2, [6] = 4, [8] = 5, [10] = 6, [13] = 8, [15] = 9, [18] = 11, [20] = 12, [22] = 13}
local codes = {0x2c, 0x1f, 0x2d, 0x20, 0x2e, 0x2f, 0x22, 0x30, 0x23, 0x31, 0x24, 0x32,
               0x10, 0x03, 0x11, 0x04, 0x12, 0x13, 0x06, 0x14, 0x07, 0x15, 0x08, 0x16, 0x17}
local note_for_code = {}; for i, code in ipairs(codes) do note_for_code[code] = i - 1 end
local View = {}; View.__index = View
local function control_text(spec, value)
  if spec[1] == "lfo_rate" then return string.format("Vibrato rate: %.1f Hz", value / 10) end
  return spec[2] .. ": " .. value .. spec[6]
end
local function log(message) io.write("luasynth: " .. message .. "\n"); io.flush() end
function View:base() return (self.octave:value() + 1) * 12 end
function View:press(id, note)
  if self.held[id] == note then return end
  self:release_key(id)
  self.held[id] = note
  self.counts[note] = (self.counts[note] or 0) + 1
  if self.counts[note] == 1 then self.engine:note_on(note, self.velocity:value()) end
  self.keys:invalidate()
  if not self.engine.remote then log(string.format("note %d on, voices %d", note, self.engine:active())) end
end
function View:release_key(id)
  local note = self.held[id]
  if not note then return end
  self.held[id] = nil
  self.counts[note] = self.counts[note] - 1
  if self.counts[note] == 0 then self.counts[note] = nil; self.engine:note_off(note) end
  self.keys:invalidate()
end
function View:release_all()
  self.held, self.counts, self.mouse_down = {}, {}, false
  self.engine:all_off()
  self.sustain:value(0)
  self.keys:invalidate()
end
function View:panic()
  self.demo_on = false
  self.demo:text("Demo")
  self:release_all(); self.engine:panic()
  self.message = "All sound stopped"
  log("panic")
end
function View:apply(patch, title)
  local ok, err = self.engine:apply(patch)
  if not ok then self.message = err; return nil, err end
  self:release_all()
  self.updating = true
  for _, spec in ipairs(self.synth.controls) do
    local key, value = spec[1], self.engine.params[spec[1]]
    if key == "wave_a" or key == "wave_b" then self.controls[key]:select(value)
    else self.controls[key]:value(value) end
    if self.labels[key] then self.labels[key]:text(control_text(spec, value)) end
  end
  self.updating = false
  self.message = title or "Patch loaded"
  log(self.message)
  return true
end
function View:save(path)
  local data, err = self.synth.encode(self.engine.params)
  if not data then return nil, err end
  local f; f, err = io.open(path, "wb")
  if not f then return nil, err end
  local ok; ok, err = f:write(data)
  local closed, close_err = f:close()
  if not ok or not closed then return nil, err or close_err end
  self.message = "Saved " .. path
  return true
end
function View:load(path)
  local f, err = io.open(path, "rb")
  if not f then return nil, err end
  local data = f:read(8193); f:close()
  local patch; patch, err = self.synth.decode(data or "")
  if not patch then return nil, err end
  return self:apply(patch, "Loaded " .. path)
end
function View:audio_error(message)
  if self.watch then self.watch:remove(); self.watch = nil end
  self.message = "Audio stopped: " .. tostring(message)
  self.audio_failed = true
  log(self.message)
end
function View:update_status()
  local xruns = self.engine.xruns or 0
  local text = string.format("%s  |  %d/8 voices  |  Peak %d%%  |  Underruns %d", self.message, self.engine:active(), math.floor(self.engine.peak * 100), xruns)
  if self.status_text ~= text then
    self.status_text = text
    self.status:invalidate()
  end
end
function View:close()
  if self.closed then return end
  self.closed = true
  if self.watch then self.watch:remove(); self.watch = nil end
  if self.timer then self.timer:remove(); self.timer = nil end
  if self.engine.remote then self.engine:close() end
  log("closed, underruns " .. (self.engine.xruns or 0))
  self.app:destroy()
end
function View:run()
  local ok, code = xpcall(function() return self.app:run() end, debug.traceback)
  self:close()
  if not ok then error(code) end
  return code
end
function View:note_at(x, y)
  local w, h = self.keys:size()
  if x < 0 or y < 0 or x >= w or y >= h then return nil end
  local kw = w / #white
  if y < h * 0.62 then
    for note, edge in pairs(black) do
      if x >= (edge - 0.30) * kw and x < (edge + 0.30) * kw then return self:base() + note end
    end
  end
  return self:base() + white[math.min(#white, math.floor(x / kw) + 1)]
end
function M.new(synth, engine)
  local self = setmetatable({synth = synth, engine = engine or Controller.new(synth), controls = {}, labels = {},
    held = {}, counts = {}, message = "Bright keys", demo_step = 0}, View)
  self.engine.on_note = function(note, voices)
    log(string.format("note %d on, voices %d", note, voices))
  end
  self.app = assert(gui.app())
  self.win = self.app:window(860, 670, "Lua Synthesizer")
  local top = gui.hbox(self.win)
  gui.label(top, "Preset")
  self.presets = gui.combobox(top):hint(160, 0)
  for _, preset in ipairs(synth.presets) do self.presets:add(preset.name) end
  self.presets:select(2)
  local save, load = gui.button(top, "Save patch"), gui.button(top, "Load patch")
  self.demo = gui.button(top, "Demo")
  local panic = gui.button(top, "Panic")
  top = gui.hbox(self.win)
  gui.label(top, "Octave")
  self.octave = gui.spinner(top, 1, 6, 4):hint(64, 0)
  gui.label(top, "Velocity")
  self.velocity = gui.spinner(top, 1, 127, 100):hint(64, 0)
  self.sustain = gui.checkbox(top, "Sustain")
  local grid = gui.grid(self.win):stretch(1, 0):gridstretch(-1, 1, 1):gridstretch(-1, 3, 1)
  for i, spec in ipairs(synth.controls) do
    local key, row, col = spec[1], (i - 1) // 2, ((i - 1) % 2) * 2
    local value = self.engine.params[key]
    local label = gui.label(grid, spec[2]):grid(row, col)
    local control
    if key == "wave_a" or key == "wave_b" then
      control = gui.combobox(grid):grid(row, col + 1)
      for _, wave in ipairs(synth.waves) do control:add(wave) end
      control:select(value)
      control:on("changed", function(_, e)
        if not self.updating then
          local ok, err = self.engine:set(key, e.index)
          if ok then self.message = "Custom patch" else self:audio_error(err) end
        end
      end)
    else
      control = gui.slider(grid, spec[3], spec[4], value):grid(row, col + 1)
      self.labels[key] = label
      label:text(control_text(spec, value))
      control:on("changed", function(_, e)
        if not self.updating then
          local ok, err = self.engine:set(key, e.value)
          if not ok then self:audio_error(err); return end
          label:text(control_text(spec, e.value)); self.message = "Custom patch"
        end
      end)
    end
    control:id(key)
    self.controls[key] = control
  end
  self.scope = gui.canvas(self.win):hint(0, 88):stretch(1, 1)
  self.scope:on("paint", function(w, p)
    local width, height = w:size()
    p:fill(0, 0, width, height, 0x151d2b)
    p:text(10, 6, "OUTPUT  /  48 kHz STEREO  /  8 VOICES", 0x9bb7cc)
    local mid = (height + 24) // 2
    p:line(6, mid, width - 6, mid, 0x334356)
    local points = self.engine.scope
    local px, py
    for i, value in ipairs(points) do
      local x = 6 + (i - 1) * (width - 12) // math.max(1, #points - 1)
      local y = math.floor(mid - value * (height - 28) * 0.8)
      if px then p:line(px, py, x, y, 0x65dec7) end
      px, py = x, y
    end
  end)
  self.keys = gui.canvas(self.win):hint(0, 108):id("keyboard")
  self.keys:on("paint", function(w, p)
    local width, height = w:size()
    local kw = width / #white
    for i, note in ipairs(white) do
      local x, right = math.floor((i - 1) * kw), math.floor(i * kw)
      p:fill(x, 0, right - x, height, self.counts[self:base() + note] and 0x6ad5c1 or 0xf4f2ec)
      p:frame(x, 0, right - x, height, 0x526070)
      local text = names[note % 12 + 1] .. ((self:base() + note) // 12 - 1)
      p:text(x + 7, height - 22, text, 0x243341)
    end
    for note, edge in pairs(black) do
      local x, right = math.floor((edge - 0.30) * kw), math.floor((edge + 0.30) * kw)
      p:fill(x, 0, right - x, math.floor(height * 0.62), self.counts[self:base() + note] and 0x299b87 or 0x263443)
      p:frame(x, 0, right - x, math.floor(height * 0.62), 0x101820)
    end
  end)
  self.keys:on("press", function(_, e)
    if e.button & 1 == 0 then return false end
    self.mouse_down = true
    local note = self:note_at(e.x, e.y)
    if note then self:press("mouse", note) end
    return true
  end)
  self.keys:on("motion", function(_, e)
    if self.mouse_down and e.button & 1 ~= 0 then
      local note = self:note_at(e.x, e.y)
      if note then self:press("mouse", note) else self:release_key("mouse") end
    end
    return true
  end)
  self.keys:on("release", function() self.mouse_down = false; self:release_key("mouse"); return true end)
  local function shortcut(_, e)
    if e.code == 0x01 then self:panic(); return true end
    if e.code == 0x10 and e.mods & gui.mod.ctrl ~= 0 then self.app:quit(0); return true end
    return false
  end
  self.keys:on("key", function(w, e)
    if shortcut(w, e) then return true end
    if e.mods & (gui.mod.ctrl | gui.mod.alt) ~= 0 then return false end
    if e.code == 0x39 then self.engine:set_pedal(true); self.sustain:value(1); return true end
    if e.code == 0x1a or e.code == 0x1b then
      self:release_all(); self.octave:value(math.max(1, math.min(6, self.octave:value() + (e.code == 0x1a and -1 or 1)))); return true
    end
    local note = note_for_code[e.code]
    if note then self:press("key" .. e.code, self:base() + note); return true end
    return false
  end)
  self.keys:on("keyup", function(_, e)
    if e.code == 0x39 then self.engine:set_pedal(false); self.sustain:value(0)
    else self:release_key("key" .. e.code) end
    return true
  end)
  self.win:on("key", shortcut)
  self.win:on("focus", function(_, e) if e.value == 0 then self:release_all() end end)
  self.win:on("close", function() self.app:quit(0); return true end)
  self.sustain:on("toggled", function(_, e) self.engine:set_pedal(e.value ~= 0); self.keys:focus() end)
  self.octave:on("changed", function() self:release_all(); self.keys:focus() end)
  self.presets:on("changed", function(_, e) self:apply(synth.presets[e.index], synth.presets[e.index].name); self.keys:focus() end)
  panic:on("clicked", function() self:panic(); self.keys:focus() end)
  self.demo:on("clicked", function()
    self:release_all(); self.demo_on = not self.demo_on; self.next_demo = 0
    self.demo:text(self.demo_on and "Stop demo" or "Demo"); self.keys:focus()
  end)
  local patch_path = (os.getenv("HOME") or "/home") .. "/luasynth.lsynth"
  save:on("clicked", function()
    self:release_all()
    local path = self.app:prompt("Save Lua Synthesizer patch", "File path", patch_path)
    if path then local ok, err = self:save(path); if not ok then self.message = "Save failed: " .. tostring(err) end end
    self.keys:focus()
  end)
  load:on("clicked", function()
    self:release_all()
    local path = self.app:prompt("Load Lua Synthesizer patch", "File path", patch_path)
    if path then local ok, err = self:load(path); if not ok then self.message = "Load failed: " .. tostring(err) end end
    self.keys:focus()
  end)
  gui.label(self.win, "Lower: Z S X D C V G B H N J M    Upper: Q 2 W 3 E R 5 T 6 Y 7 U I")
  gui.label(self.win, "Space: sustain    [ / ]: octave    Esc: panic    Ctrl+Q: close    Click the keyboard to play")
  -- A label's text setter requests layout and repaints the entire window.
  -- Keep live meters in a fixed canvas so a peak change cannot hold up audio
  -- behind a full-window layout/paint (particularly expensive at scale 2).
  self.status_text = "Connecting to audio..."
  self.status = gui.canvas(self.win):hint(0, 20)
  self.status:on("paint", function(w, p)
    local width, height = w:size()
    local colors = self.app:theme().color
    p:fill(0, 0, width, height, colors.window)
    p:text(0, 2, self.status_text, colors.text)
  end)
  self.keys:focus()
  if self.engine.remote then
    self.watch = self.app:watch(self.engine:fd(), "r", function()
      local ok, err = self.engine:dispatch()
      if not ok then self:audio_error(err) end
    end)
  end
  self.timer = self.app:timer(100, true, function()
    if not self.keys:focused() and next(self.held) and not self.demo_on then self:release_all() end
    if self.demo_on and sys.uptime() >= self.next_demo then
      for i = 1, 3 do self:release_key("demo" .. i) end
      local chords = {{0, 4, 7}, {5, 9, 12}, {7, 11, 14}, {0, 7, 12}}
      self.demo_step = self.demo_step % #chords + 1
      for i, n in ipairs(chords[self.demo_step]) do self:press("demo" .. i, self:base() + n) end
      self.next_demo = sys.uptime() + 700
    end
    if self.engine.error and not self.audio_failed then self:audio_error(self.engine.error) end
    self:update_status()
    -- Key transitions already invalidate the keyboard; a held chord is static.
    self.scope:invalidate()
  end)
  self:update_status()
  log("ready, 8 voices")
  return self
end
return M
