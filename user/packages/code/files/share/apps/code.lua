-- Code: a source editor with a run panel, written on the gui, sys and fs
-- modules. Languages are entries of the `languages` table below: the
-- file extensions, the highlighter (a name or a table for
-- editor:highlight), the command that runs a file, and a pattern that
-- finds the definitions listed in the outline. $HOME/.config/code.lua
-- may add or change entries and settings: it is loaded with the tables
-- as `languages` and `settings`.
--
--     lua /home/.local/share/apps/code.lua [file]

local gui = require "gui"
local sys = require "sys"
local fs = require "fs"

-- Settings that $HOME/.config/code.lua may change as well.
local settings = {
  font = "/etc/fonts/DejaVuSansMono.ttf",
  font_px = 13,
}

local languages = {
  lua = {
    name = "Lua", extensions = { "lua" }, highlight = "lua",
    run = { "lua", "$FILE" },
    definition = "^%s*local%s+function%s+([%w_.:]+)", definition2 = "^%s*function%s+([%w_.:]+)",
    template = "print(\"hello\")\n",
  },
  c = {
    name = "C", extensions = { "c", "h" }, highlight = "c",
    run = { "tcc", "-run", "$FILE" },
    definition = "^[%w_]+[%s%*]+([%w_]+)%s*%(",
    template = "#include <stdio.h>\n\nint main(void)\n{\n    printf(\"hello\\n\");\n    return 0;\n}\n",
  },
  sh = {
    name = "Shell", extensions = { "sh" }, highlight = "sh",
    run = { "sh", "$FILE" },
    definition = "^%s*([%w_]+)%s*%(%s*%)",
    template = "#!/bin/sh\necho hello\n",
  },
}

local home = os.getenv("HOME") or "/home"
do
  local f = io.open(home .. "/.config/code.lua", "r")
  if f then
    f:close()
    local chunk, err = loadfile(home .. "/.config/code.lua", "t", setmetatable({ languages = languages, settings = settings }, { __index = _G }))
    if chunk then chunk() else io.stderr:write("code: " .. err .. "\n") end
  end
end

local function language_of(path)
  local ext = path and path:match("%.([%w]+)$")
  if ext then
    for _, lang in pairs(languages) do
      for _, e in ipairs(lang.extensions) do
        if e == ext then return lang end
      end
    end
  end
  return nil
end

local function log(fmt, ...)
  io.write(string.format("code: " .. fmt .. "\n", ...))
  io.flush()
end

local app = assert(gui.app())
local win = app:window(720, 520, "Code")
local path, language = nil, nil
local needle = ""
local child, child_fd, child_watch = nil, nil, nil

-- menus
local bar = gui.menubar(win)
local file_menu = gui.menu(bar, "File")
local edit_menu = gui.menu(bar, "Edit")
local run_menu = gui.menu(bar, "Run")
local view_menu = gui.menu(bar, "View")

-- tool bar
local tools = gui.toolbar(win)

-- the editor and the output pane in a vertical split, the outline beside them
local split = gui.splitpane(win, false)
local left = gui.splitpane(split, true)
local editor = gui.editor(left)
local output = gui.editor(left):readonly(true)
for _, pane in ipairs({ editor, output }) do
  local ok, err = pane:font(settings.font, settings.font_px)
  if not ok then io.stderr:write("code: font " .. settings.font .. ": " .. tostring(err) .. "\n") end
end
local outline = gui.table(split)
split:position(540)
left:position(330)

-- status bar
local status = gui.statusbar(win)
local status_file = status:field(1)
local status_pos = status:field(0)
local status_run = status:field(0)

local definitions = {}

local function update_title()
  local name = path or "untitled"
  win:title(name .. (editor:modified() and " *" or "") .. " - Code")
  status_file:text(name .. (language and ("  [" .. language.name .. "]") or ""))
end

local function update_cursor()
  local line, col = editor:cursor()
  status_pos:text(string.format("%d:%d", line, col))
end

local function update_outline()
  definitions = {}
  if language and language.definition then
    for i = 1, editor:lines() do
      local text = editor:line(i)
      local name = text:match(language.definition) or (language.definition2 and text:match(language.definition2))
      if name then definitions[#definitions + 1] = { name = name, line = i } end
    end
  end
  outline:refresh()
end

outline:model {
  columns = 2,
  rows = function(parent) return parent == -1 and #definitions or 0 end,
  child = function(parent, index) return index end,
  cell = function(row, col)
    local d = definitions[row]
    if not d then return "" end
    return col == 1 and d.name or tostring(d.line)
  end,
  header = function(col) return col == 1 and "Definition" or "Line" end,
}
outline:on("activate", function(w, e)
  local d = definitions[e.row]
  if d then editor:go(d.line, 1); editor:focus() end
  return true
end)

local function set_language(lang)
  language = lang
  editor:highlight(lang and lang.highlight or nil)
  update_outline()
  update_title()
end

local function load(name)
  local text, err = fs.read(name)
  if not text then return nil, err end
  editor:text(text)
  path = name
  set_language(language_of(name))
  log("opened %s (%s)", name, language and language.name or "plain text")
  return true
end

local function save()
  if not path then return nil end
  local ok, err = fs.write(path, editor:text())
  if not ok then return nil, err end
  editor:modified(false)
  update_title()
  update_outline()
  log("saved %s", path)
  return true
end

local function error_box(text)
  app:dialog("Error", text, { "OK" })
end

local function save_as()
  local name = app:prompt("Save as", "File:", path or "")
  if not name or name == "" then return false end
  path = name
  set_language(language_of(name))
  local ok, err = save()
  if not ok then error_box("The file cannot be written: " .. tostring(err)) end
  return ok
end

local function new_file(lang)
  path = nil
  editor:text(lang and lang.template or "")
  editor:modified(false)
  set_language(lang)
end

local function open_file()
  local name = app:prompt("Open", "File:", path or "")
  if name and name ~= "" then
    local ok, err = load(name)
    if not ok then error_box("The file cannot be opened: " .. tostring(err)) end
  end
end

-- running
local function append_output(text)
  local current = output:text()
  output:text(current == "" and text or current .. text)
  output:go(output:lines(), 1)
end

local function stop_run()
  if child then
    sys.kill(child, "TERM")
    log("stopped %d", child)
  end
end

local function finish_run()
  if child_watch then child_watch:remove(); child_watch = nil end
  if child_fd then sys.close(child_fd); child_fd = nil end
  local what, code = sys.wait(child)
  child = nil
  local line = what == "exit" and string.format("[exited with status %d]", code)
      or string.format("[terminated by signal %d]", code)
  append_output(line .. "\n")
  status_run:text("")
  log("run %s %d", what, code)
end

local function run_file()
  if child then return end
  if not language or not language.run then
    error_box("There is no way to run this kind of file.")
    return
  end
  if not path or editor:modified() then
    if not path then
      if not save_as() then return end
    else
      save()
    end
  end
  local argv = {}
  for i, a in ipairs(language.run) do argv[i] = a:gsub("%$FILE", path) end
  output:text("")
  local pid, fd = sys.spawn_pipe(table.unpack(argv))
  if not pid then
    error_box("The program cannot be started: " .. tostring(fd))
    return
  end
  child, child_fd = pid, fd
  status_run:text("running " .. argv[1])
  log("run: %s", table.concat(argv, " "))
  child_watch = app:watch(fd, "r", function()
    local data = sys.read(fd)
    if data == nil then
      finish_run()
    elseif data ~= "" then
      append_output(data)
      for line in data:gmatch("[^\n]+") do log("output: %s", line) end
    end
  end)
end

-- commands
local function command(menu, text, icon, fn, key, mods)
  local item = menu:menuitem(text, icon):on("clicked", function() fn(); return true end)
  if key then item:accel(key, mods or gui.mod.ctrl) end
  return item
end

command(file_menu, "New", "new", function() new_file(language) end, gui.key.n)
for _, name in ipairs({ "lua", "c", "sh" }) do
  local lang = languages[name]
  command(file_menu, "New " .. lang.name .. " file", nil, function() new_file(lang) end)
end
command(file_menu, "Open...", "open", open_file, gui.key.o)
command(file_menu, "Save", "save", function()
  if not path then save_as() elseif not save() then error_box("The file cannot be written.") end
end, gui.key.s)
command(file_menu, "Save as...", nil, save_as)
file_menu:separator()
command(file_menu, "Quit", "quit", function() win:close() end, gui.key.q)
command(edit_menu, "Undo", nil, function() editor:undo() end, gui.key.z)
command(edit_menu, "Redo", nil, function() editor:redo() end, gui.key.y)
edit_menu:separator()
command(edit_menu, "Find...", "search", function()
  local text = app:prompt("Find", "Text:", needle)
  if text and text ~= "" then
    needle = text
    if not editor:search(needle, true) then status_run:text("not found") end
  end
  editor:focus()
end, gui.key.f)
command(edit_menu, "Find next", nil, function()
  if needle ~= "" and not editor:search(needle, true) then status_run:text("not found") end
end, gui.key.g)
command(run_menu, "Run", nil, run_file, gui.key.f5, 0)
command(run_menu, "Stop", nil, stop_run, gui.key.f6, 0)
command(run_menu, "Clear output", nil, function() output:text("") end)
local wrap_item = command(view_menu, "Wrap lines", nil, function() end)
wrap_item:on("clicked", function(w) editor:wrap(not (w.wrapped or false)); w.wrapped = not (w.wrapped or false); return true end)
local numbers_on = true
editor:numbers(true)
command(view_menu, "Line numbers", nil, function() numbers_on = not numbers_on; editor:numbers(numbers_on) end)
command(view_menu, "Outline", nil, function() outline:visible(not outline:visible()); win:relayout() end)

tools:tool("new", "New"):on("clicked", function() new_file(language); return true end)
tools:tool("open", "Open"):on("clicked", function() open_file(); return true end)
tools:tool("save", "Save"):on("clicked", function() if not path then save_as() else save() end; return true end)
tools:tool("search", "Find"):on("clicked", function()
  local text = app:prompt("Find", "Text:", needle)
  if text and text ~= "" then needle = text; editor:search(needle, true) end
  return true
end)
gui.separator(tools)
gui.button(tools, "Run"):tip("Run the file (F5)"):on("clicked", function() run_file(); return true end)
gui.button(tools, "Stop"):tip("Stop the program (F6)"):on("clicked", function() stop_run(); return true end)

editor:on("changed", function() update_title(); update_cursor() end)
editor:on("cursor", function() update_cursor() end)
win:on("close", function()
  if child then stop_run() end
  if not editor:modified() then return false end
  local r = app:dialog("Unsaved changes", "Save the changes before closing?", { "Save", "Discard", "Cancel" })
  if r == 1 then
    if not path then return not save_as() end
    save()
  end
  return r == 3 or r == nil
end)

if arg[1] then
  local ok, err = load(arg[1])
  if not ok then
    path = arg[1]
    set_language(language_of(arg[1]))
    log("new file %s", arg[1])
  end
else
  new_file(languages.lua)
end
update_title()
update_cursor()
editor:focus()
os.exit(app:run())
