-- This boot test checks the image, clipboard and layer bindings of the
-- gui module on the compositor (tests/cases/gui_lua_bindings). The
-- window's canvas shows a two pixel image enlarged to its size, blue on
-- the left and orange on the right, with an icon drawn at twice its size
-- over it. The clipboard passes a text to a second client
-- (/etc/tests/luaclip.lua) and back. A layer window then covers the top
-- edge in purple. The kernel test finds the canvas and the layer on the
-- screen and presses Escape, which closes the layer window and ends the
-- program. Every check logs a line, and a timer ends the program with
-- status 3 if Escape never arrives.
local gui = require "gui"
local sys = require "sys"

local failures = 0
local function check(cond, what)
  if cond then
    print("luabind: " .. what)
  else
    failures = failures + 1
    print("luabind: a check failed (" .. what .. ")")
  end
end

local app = assert(gui.app())
local win = app:window(260, 180, "lua bindings"):padding(0)
local canvas = gui.canvas(win)

-- The program loads a PNG icon and an SVG icon, provokes the two errors
-- and builds an image from a pixel string.
local icon, err = gui.image("/usr/share/icons/save.png")
check(icon ~= nil, "the PNG icon loaded" .. (icon and "" or ", " .. err))
local iw, ih = icon:size()
check(iw == 16 and ih == 16 and icon:scale() == 1, "the PNG icon is " .. iw .. "x" .. ih)
local svg = gui.image("/usr/share/icons/clock.svg", 24)
local sw, sh = svg:size()
check(sw == 24 and sh == 24 and svg:pixel(0, 0) ~= nil, "the SVG icon is " .. sw .. "x" .. sh)
local none, msg, errno = gui.image("/usr/share/icons/missing.png")
check(none == nil and errno == 2 and msg:find("missing.png", 1, true), "a missing image is reported")
local f = assert(io.open("/tmp/luabind.png", "wb"))
f:write("this is not a PNG file")
f:close()
none, msg = gui.image("/tmp/luabind.png")
os.remove("/tmp/luabind.png")
check(none == nil and msg:find("not a valid PNG file", 1, true), "a malformed image is reported")
local swatch = gui.from_pixels(2, 1, string.pack("<I4I4", 0xff2060c0, 0xffc06020))

-- The check below looks for an opaque pixel of the icon in the window.
local ox, oy
for y = 0, ih - 1 do
  for x = 0, iw - 1 do
    if not ox and icon:pixel(x, y) >> 24 == 0xff then ox, oy = x, y end
  end
end

canvas:on("paint", function(w, p)
  local cw, ch = w:size()
  p:image(swatch, 0, 0, cw, ch)
  p:image(icon, 4, 4, 32, 32)
end)

local bar
local function open_layer()
  local screen_w = app:screen()
  bar = assert(app:layer(0, 28, { layer = "top", anchor = "top left right", exclusive = 28,
                                  namespace = "luabind" }))
  bar:padding(0)
  gui.canvas(bar):on("paint", function(w, p)
    local cw, ch = w:size()
    p:fill(0, 0, cw, ch, 0x7030a0)
  end)
  local bw, bh = bar:size()
  check(bw == screen_w and bh == 28, "the layer window spans the screen width")
  print("luabind: the layer window is shown")
end

-- The parent sets the clipboard, the child reads and replaces it, and
-- the parent reads the child's text back.
local function read_back(tries)
  local text = app:clipboard()
  if text == "from child" then
    check(true, "the clipboard came back from the child")
    open_layer()
  elseif tries > 0 then
    app:timer(200, false, function() read_back(tries - 1) end)
  else
    check(false, "the clipboard contained " .. tostring(text) .. " instead of the child's text")
    open_layer()
  end
end

local function run_child()
  local pid, fd = sys.spawn_pipe("/bin/lua", "/etc/tests/luaclip.lua")
  check(pid ~= nil, "the clipboard child started")
  local output = {}
  local watch
  watch = app:watch(fd, "r", function()
    local data = sys.read(fd)
    if data == nil then
      watch:remove()
      sys.close(fd)
      local how, code = sys.wait(pid)
      for line in table.concat(output):gmatch("[^\n]+") do print(line) end
      check(how == "exit" and code == 0, "the clipboard child exited")
      read_back(25)
    elseif data ~= "" then
      output[#output + 1] = data
    end
  end)
end

local started = false
win:on("focus", function(w, e)
  if e.value ~= 1 or started then return end
  started = true
  gui.test.paint(win)
  local cw, ch = canvas:size()
  local cx, cy = canvas:pos()
  check(gui.test.pixel(win, cx + 2, cy + ch - 2) == 0x2060c0 and
        gui.test.pixel(win, cx + cw - 2, cy + ch - 2) == 0xc06020, "the image is enlarged on the canvas")
  check(ox ~= nil and gui.test.pixel(win, cx + 4 + 2 * ox, cy + 4 + 2 * oy) == icon:pixel(ox, oy) & 0xffffff,
        "the icon is drawn at twice its size")
  check(app:clipboard("from parent") == app, "the clipboard is set")
  run_child()
end)

win:on("key", function(w, k)
  if k.code ~= gui.key.esc then return end
  if bar then
    bar:close()
    app:timer(100, false, function()
      check(not pcall(bar.size, bar), "the layer window closed")
      app:quit(failures == 0 and 0 or 1)
    end)
  else
    app:quit(4)
  end
  return true
end)

app:timer(40000, false, function() print("luabind: Escape never arrived") app:quit(3) end)
os.exit(app:run())
