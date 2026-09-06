-- Boot test of the gui module on the compositor: a window with a canvas
-- in one colour, closed by Escape. A timer ends the program with status
-- 3 if the key never arrives.
local gui = require "gui"
local app = assert(gui.app())
local win = app:window(240, 160, "lua gui"):padding(0)
local canvas = gui.canvas(win)
canvas:on("paint", function(w, p)
  local cw, ch = w:size()
  p:fill(0, 0, cw, ch, 0x20a040)
  p:text(8, 8, "lua", 0xffffff)
end)
win:on("key", function(w, k)
  if k.code == gui.key.esc then app:quit(0); return true end
end)
app:timer(8000, false, function() app:quit(3) end)
os.exit(app:run())
