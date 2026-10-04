-- Host unit test of the gui module over the fake client of libgui:
-- layout, signals, painting, timers and the lifetime rules. Run by
-- make check-lua.
local gui = require "gui"

local function check(cond, name)
  if cond then print("ok " .. name) else error("failed: " .. name, 2) end
end

local app = assert(gui.app())
local theme = app:theme()
check(type(theme.color.window) == "number" and theme.metric.padding > 0 and theme.scale > 0, "theme")

-- layout
local win = app:window(200, 100, "box")
local l1 = gui.label(win, "one")
local l2 = gui.label(win, "two"):stretch(0, 1)
gui.test.paint(win)
local pad, sp = theme.metric.padding, theme.metric.spacing
local x1, y1 = l1:pos()
local w1, h1 = l1:size()
local x2, y2 = l2:pos()
local _, h2 = l2:size()
check(x1 == pad and y1 == pad and w1 == 200 - 2 * pad, "label one placed")
check(y2 == y1 + h1 + sp and y2 + h2 == 100 - pad, "stretched label fills to the bottom")
check(l1:text() == "one" and l1:class() == "label" and l1:parent() == win and l1:window() == win, "widget accessors")
check(tostring(l1):match("^label: "), "tostring")
win:close()
gui.test.paint(win)

-- signals and the same object identity
win = app:window(200, 200, "signals")
local clicks, seen = 0, nil
local b = gui.button(win, "Press"):id("press")
b:on("clicked", function(w, c) clicks = clicks + 1; seen = w; return true end)
check(win:find("press") == b, "find returns the same object")
gui.test.paint(win)
local bx, by = b:pos()
local bw, bh = b:size()
gui.test.mouse(win, "down", bx + bw // 2, by + bh // 2, 1)
gui.test.mouse(win, "up", bx + bw // 2, by + bh // 2, 0)
check(clicks == 1 and seen == b, "clicked handler ran with the button")

local field = gui.textfield(win, "")
local changes, last = 0, nil
field:on("changed", function(w, c) changes = changes + 1; last = c.text end)
field:focus()
gui.test.key(win, 0, string.byte("h"))
gui.test.key(win, 0, string.byte("i"))
check(changes == 2 and last == "hi" and field:text() == "hi", "text field changed signal and text")

local keys = {}
win:on("key", function(w, k) keys[#keys + 1] = k.code; return true end)
gui.test.key(win, gui.key.f5, 0)
check(keys[1] == gui.key.f5, "window key signal")
check(gui.key.a == 30 and gui.key.up == 103 and gui.mod.ctrl == 2, "key tables")

-- painting
local canvas = gui.canvas(win):stretch(1, 1)
local painted, retained = 0, nil
canvas:on("paint", function(w, p)
  painted = painted + 1
  retained = p
  local cw, ch = w:size()
  p:fill(0, 0, cw, ch, gui.rgb(0x20, 0xa0, 0x40))
  p:text(4, 4, "canvas", 0xffffff)
  check(p:text_width("canvas") > 0 and p:text_height() > 0, "painter text metrics")
  local cx, cy, ccw, cch = p:clip()
  check(ccw == cw and cch == ch, "painter clip is the widget")
end)
gui.test.paint(win)
local cx, cy = canvas:pos()
local cw, ch = canvas:size()
check(painted == 1 and gui.test.pixel(win, cx + cw - 2, cy + ch - 2) == 0x20a040, "canvas painted")
check(not pcall(retained.fill, retained, 0, 0, 1, 1, 0), "painter is closed after the handler")
canvas:invalidate()
local dx, dy, dw, dh = gui.test.paint(win)
check(painted == 2 and dx == cx and dy == cy and dw == cw and dh == ch, "invalidate repaints the canvas only")

-- close consumed, then destroyed
local closes = 0
win:on("close", function(w) closes = closes + 1; return closes == 1 end)
gui.test.close(win)
app:step(0)
check(closes == 1 and b:text() == "Press", "first close was consumed")
gui.test.close(win)
app:step(0)
check(closes == 2 and not pcall(b.text, b), "destroyed widget raises")

-- timers and steps
local ticks = 0
local once = 0
local t = app:timer(5, true, function() ticks = ticks + 1 end)
app:timer(5, false, function() once = once + 1 end)
local deadline = os.clock() + 2
while ticks < 3 and os.clock() < deadline do app:step(10) end
t:remove()
check(ticks >= 3 and once == 1, "repeating and one shot timers")
local after = ticks
app:step(20)
check(ticks == after, "removed timer stopped")

-- other constructors
win = app:window(300, 300, "widgets")
local vb = gui.vbox(win)
local cb = gui.checkbox(vb, "check")
local combo = gui.combobox(vb):add("a"):add("b")
local list = gui.listview(vb):add("x"):add("y"):add("z")
local tabs = gui.tabs(vb)
local page = tabs:page("first")
gui.label(page, "in page")
local spin = gui.spinner(vb, 0, 10, 5)
local slide = gui.slider(vb, 0, 100)
local grid = gui.grid(vb)
gui.button(grid, "g"):grid(0, 0)
grid:gridstretch(0, 0, 1)
gui.test.paint(win)
check(combo:item(2) == "b" and list:count() == 3 and list:item(3) == "z", "items")
check(spin:value() == 5 and slide:value() == 0 and page:class() == "box", "values and pages")
check(cb:visible() and cb:enabled(), "flags")
cb:visible(false)
check(not cb:visible(), "hidden")
win:close()
app:step(0)

-- the editor
win = app:window(400, 300, "editor")
local ed = gui.editor(win)
ed:text("local x = 1\n-- comment\nprint(x)")
check(ed:lines() == 3 and ed:line(2) == "-- comment" and ed:line(4) == nil, "editor lines")
ed:highlight("lua"):highlight("c"):highlight("sh"):highlight(nil)
ed:highlight { keywords = { "print" }, line_comment = "--", block_comment = { "--[[", "]]" }, quotes = "\"'" }
gui.test.paint(win)
check(ed:search("print", true) and select(1, ed:cursor()) == 3, "editor search moves the cursor")
ed:go(1, 7)
local l, c = ed:cursor()
check(l == 1 and c == 7, "editor go")
check(not ed:modified(), "editor unmodified after set")
ed:modified(true)
check(ed:modified(), "editor modified flag")
check(ed:text() == "local x = 1\n-- comment\nprint(x)", "editor text round trip")
ed:wrap(true):numbers(true):readonly(true):readonly(false)
local fok, ferr = ed:font("/nonexistent/font.ttf", 14)
check(fok == nil and type(ferr) == "string", "editor font that does not exist fails")
check(ed:font(nil) == ed, "editor font back to the theme")
local ok = pcall(function() return gui.label(win, "x"):lines() end)
check(not ok, "lines on a label is an error")
win:close()
app:step(0)

-- menus, tool bar, status bar and a table with a model
win = app:window(400, 300, "views")
local mbar = gui.menubar(win)
local m = gui.menu(mbar, "File")
local hits = 0
m:menuitem("Open", "open"):on("clicked", function() hits = hits + 1; return true end)
m:separator()
m:menuitem("Quit"):accel(gui.key.q, gui.mod.ctrl):on("clicked", function() hits = hits + 10; return true end)
local tb = gui.toolbar(win)
local tool = tb:tool("save", "Save"):on("clicked", function() hits = hits + 100; return true end)
tool:icon("open"):icon(nil)
local sbar = gui.statusbar(win)
local field = sbar:field(1):text("ready")
check(field:text() == "ready" and tool:class() == "button", "status field and tool")
local rows = { { "b", 2 }, { "a", 1 }, { "c", 3 } }
local tbl = gui.table(win)
tbl:model {
  columns = 2,
  rows = function(parent) return parent == -1 and #rows or 0 end,
  cell = function(row, col) return tostring(rows[row + 1][col]) end,
  header = function(col) return col == 1 and "Name" or "Number" end,
  sort = function(col, descending)
    table.sort(rows, function(x, y) if descending then return x[col] > y[col] else return x[col] < y[col] end end)
  end,
}
gui.test.paint(win)
check(tbl:rows() == 3 and tbl:rows(1) == 0, "table rows from the model")
rows[#rows + 1] = { "d", 4 }
tbl:refresh()
check(tbl:rows() == 4, "table refreshed")
local selected
tbl:on("selected", function(w, e) selected = e.row; return true end)
tbl:selectrow(2)
tbl:column(1, 120)
check(tbl:column(1) == 120, "table column width")
gui.test.paint(win)
local pm = gui.popupmenu(win)
pm:menuitem("Here")
check(pm:class() == "menu" and m:class() == "menu" and mbar:class() == "menubar", "menu classes")
win:close()
app:step(0)

-- rgba.png is 37x23 with r = x*7, g = y*11, b = (x+y)*3 and alpha
-- 255 - x*2 (tools/genicons/genicons.py writes it).
local function rgba(x, y)
  return ((255 - x * 2) & 255) << 24 | (x * 7 & 255) << 16 | (y * 11 & 255) << 8 | ((x + y) * 3 & 255)
end
local img = assert(gui.image("lib/libgui/tests/data/rgba.png"))
local iw, ih = img:size()
check(iw == 37 and ih == 23 and img:scale() == 1, "the PNG image has its size")
check(img:pixel(0, 0) == rgba(0, 0) and img:pixel(5, 9) == rgba(5, 9) and img:pixel(36, 22) == rgba(36, 22), "the PNG pixels follow the formulas")
local data = img:pixels()
check(#data == 37 * 23 * 4 and string.unpack("<I4", data, (9 * 37 + 5) * 4 + 1) == rgba(5, 9), "the pixel string contains the pixels")
check(not pcall(img.pixel, img, 37, 0) and not pcall(img.pixel, img, 0, -1), "a pixel outside the image raises an error")
check(tostring(img) == "image: 37x23", "tostring names the size")
local none, msg, errno = gui.image("lib/libgui/tests/data/missing.png")
check(none == nil and msg:find("missing.png: ", 1, true) and math.type(errno) == "integer", "a missing file returns nil, the message and the errno")
local bad = os.tmpname()
local f = assert(io.open(bad .. ".png", "wb"))
f:write("not a png file")
f:close()
none, msg = gui.image(bad .. ".png")
check(none == nil and msg:find("not a valid PNG file", 1, true), "a malformed PNG file is reported")
f = assert(io.open(bad .. ".svg", "wb"))
f:write("<html><body>no drawing</body></html>")
f:close()
none, msg = gui.image(bad .. ".svg")
check(none == nil and msg:find("not a valid SVG file", 1, true), "a malformed SVG file is reported")
os.remove(bad)
os.remove(bad .. ".png")
os.remove(bad .. ".svg")
local svg = assert(gui.image("lib/libgui/tests/data/shape.svg", 32))
local sw, sh = svg:size()
check(sw == 32 and sh == 32 and svg:scale() == 1 and svg:pixel(16, 16) == 0xffff0000, "the SVG file is rendered at the requested size")
check(not pcall(gui.image, "lib/libgui/tests/data/shape.svg", 0), "the SVG size is checked")

local px = gui.from_pixels(2, 2, string.pack("<I4I4I4I4", 0xffff0000, 0xff00ff00, 0xff0000ff, 0x80ffffff))
check(px:pixel(1, 0) == 0xff00ff00 and px:pixel(1, 1) == 0x80ffffff, "from_pixels stores the pixels")
check(not pcall(gui.from_pixels, 2, 2, "short") and not pcall(gui.from_pixels, 0, 2), "from_pixels checks its arguments")
check(px:pixel(0, 0, 0xff123456) == px and px:pixel(0, 0) == 0xff123456, "a pixel store returns the image")
check(gui.from_pixels(3, 1):pixel(2, 0) == 0, "a blank image is transparent")
local quad = gui.from_pixels(2, 2, string.pack("<I4I4I4I4", 0xffff0000, 0xff00ff00, 0xff0000ff, 0xffffffff))

local oversized
win = app:window(200, 200, "images"):padding(0)
local drawn = gui.canvas(win)
drawn:on("paint", function(w, p)
  p:fill(0, 0, 200, 200, 0xffffff)
  p:image(px, 0, 0)
  p:image(px, 10, 10, 20, 20)
  p:image(img, 50, 50, 74, 46)
  p:image(quad, 150, 150, 1, 1)
  p:image(px, 160, 160, 0, 5)
  oversized = pcall(p.image, p, px, 0, 0, 100000, 100000)
end)
gui.test.paint(win)
check(oversized == false, "an oversized drawing raises an error")
check(gui.test.pixel(win, 0, 0) == 0x123456 and gui.test.pixel(win, 1, 0) == 0x00ff00, "the image is drawn at its size")
check(gui.test.pixel(win, 14, 14) == 0x123456 and gui.test.pixel(win, 25, 14) == 0x00ff00 and
      gui.test.pixel(win, 15, 25) == 0x0000ff, "the image is enlarged")
check(gui.test.pixel(win, 51, 61) == 0x00370f, "the PNG image is enlarged twice")
check(gui.test.pixel(win, 150, 150) == 0x808080, "the image is reduced by averaging")
win:close()
app:step(0)

-- The image view shows the image centred, reduced to fit, and emits paint.
win = app:window(100, 100, "view"):padding(0)
local view = gui.imageview(win, img):stretch(1, 1)
local overlay = 0
view:on("paint", function(w, p) overlay = overlay + 1 end)
gui.test.paint(win)
local vw, vh = view:size()
check(view:class() == "imageview" and vw == 100 and vh == 100, "the image view fills the window")
check(overlay == 1 and gui.test.pixel(win, 31, 38 + 5) == 0x00370f, "the image view centres the image")
win:close()
app:step(0)
win = app:window(20, 100, "narrow"):padding(0)
view = gui.imageview(win):stretch(1, 1)
gui.test.paint(win)
local back = gui.test.pixel(win, 10, 50)
view:image(quad)
gui.test.paint(win)
check(gui.test.pixel(win, 9, 49) == 0xff0000 and gui.test.pixel(win, 10, 50) == 0xffffff and
      gui.test.pixel(win, 5, 45) == back, "the image view centres a small image without enlarging it")
view:image(img)
gui.test.paint(win)
check(gui.test.pixel(win, 0, 44) == 0x000000 and gui.test.pixel(win, 10, 30) == back,
      "the image view reduces a wide image to the width")
local box = gui.checkbox(win, "c")
check(not pcall(box.image, box, img), "an image on a check box raises an error")
win:close()
app:step(0)

-- A label retains its image.
win = app:window(100, 40, "label"):padding(0)
local lab = gui.label(win, ""):image(gui.from_pixels(4, 4, string.rep(string.pack("<I4", 0xff4080c0), 16)))
collectgarbage()
collectgarbage()
gui.test.paint(win)
local lx, ly = lab:pos()
local _, lh = lab:size()
check(gui.test.pixel(win, lx + 1, ly + lh // 2) == 0x4080c0, "the label draws the image after a collection")
lab:image(nil)
gui.button(win, "b"):image(img):icon("open")
win:close()
app:step(0)

-- The fake client retains the clipboard in memory.
check(app:clipboard("copied\0text") == app and app:clipboard() == "copied\0text", "the clipboard returns the text it was given")
check(not pcall(app.clipboard, app, string.rep("x", 65537)), "the clipboard refuses a text beyond its limit")

-- The fake client gives a layer window of width 0 the screen width.
local screen_w, screen_h = app:screen()
local bar = assert(app:layer(0, 24, { layer = "top", anchor = "top left right", exclusive = 24, namespace = "bar" }))
local bw, bh = bar:size()
check(bar:class() == "window" and bw == screen_w and bh == 24 and screen_h > 0, "the layer window has the configured size")
gui.label(bar, "panel")
gui.test.paint(bar)
check(not pcall(app.layer, app, 10, 10, { layer = "middle" }), "an unknown layer raises an error")
check(not pcall(app.layer, app, 10, 10, { anchor = "top up" }), "an unknown anchor raises an error")
check(not pcall(app.layer, app, -1, 10), "a negative layer size raises an error")
local over = assert(app:layer(50, 40, { layer = "overlay", keyboard = true }))
check(select(1, over:size()) == 50, "the overlay layer has its size")
over:close()
bar:close()
app:step(0)
check(not pcall(bar.size, bar), "the closed layer window is destroyed")

app:destroy()
print("gui: done")
