-- clock: the time of day (UTC) on a canvas, redrawn every second.
local gui = require "gui"

local app = assert(gui.app())
local win = app:window(200, 100, "clock"):padding(0)
local canvas = gui.canvas(win)
canvas:on("paint", function(w, p)
  local cw, ch = w:size()
  p:fill(0, 0, cw, ch, 0xffffff)
  local text = os.date("!%H:%M:%S")
  p:text((cw - p:text_width(text)) // 2, (ch - p:text_height()) // 2, text, 0x000000)
end)
app:timer(1000, true, function() canvas:invalidate() end)
app:run()
