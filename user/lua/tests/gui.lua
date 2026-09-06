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
local painted, kept = 0, nil
canvas:on("paint", function(w, p)
  painted = painted + 1
  kept = p
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
check(not pcall(kept.fill, kept, 0, 0, 1, 1, 0), "painter is closed after the handler")
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
app:destroy()
print("gui: done")
