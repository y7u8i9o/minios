-- This program is the second client of the clipboard check in
-- luabind.lua. Once its window has the keyboard focus, which the
-- compositor requires of the owner of a selection, it reads the
-- parent's text and replaces it with its own. The parent reads the
-- lines it prints from a pipe.
local gui = require "gui"

local app = assert(gui.app())
local win = app:window(120, 60, "clipboard child")
local done = false
win:on("focus", function(w, e)
  if e.value ~= 1 or done then return end
  done = true
  local text, err = app:clipboard()
  print("luaclip: the clipboard held '" .. tostring(text or err) .. "'")
  if app:clipboard("from child") == app then
    print("luaclip: the child set the clipboard")
  else
    print("luaclip: setting the clipboard failed")
  end
  win:close()
end)
app:timer(10000, false, function() print("luaclip: the window never gained the focus") app:quit(3) end)
os.exit(app:run())
