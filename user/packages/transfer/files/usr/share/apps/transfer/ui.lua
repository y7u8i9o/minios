-- The window of Transfer (docs/design/filetransfer.md). It has the parts
-- and the texts of the window of tools/transfer.py. The client runs in the
-- worker thread client.lua and the server in the worker thread server.lua.
-- The window receives their messages through descriptor watches.
--
--     local UI = dofile("/usr/share/apps/transfer/ui.lua")
--     UI.new{ host = "10.0.2.2", port = 9103 }:run()

local gui = require "gui"
local sys = require "sys"
local fs = require "fs"
local thread = require "thread"
local mft = require "mft"

local UI = {}
UI.__index = UI

local SEP = "\0"
local ENTRY_MIME = "application/x-mft-entry"
local HERE = debug.getinfo(1, "S").source:match("^@(.*/)") or "./"

local function human_size(n)
  if n < 1024 then return string.format("%d B", n) end
  local units = { "KiB", "MiB", "GiB" }
  local v = n
  for i, unit in ipairs(units) do
    v = v / 1024
    if v < 1024 or i == #units then return string.format("%.1f %s", v, unit) end
  end
end
UI.human_size = human_size

local function format_time(mtime)
  return os.date("%Y-%m-%d %H:%M", mtime)
end

local function split(msg)
  local fields = {}
  for f in (msg .. SEP):gmatch("(.-)\0") do fields[#fields + 1] = f end
  return fields
end

-- Folders first, then files, each group in the order of the names.
local function sort_entries(entries)
  table.sort(entries, function(a, b)
    if (a.kind == "d") ~= (b.kind == "d") then return a.kind == "d" end
    local la, lb = a.name:lower(), b.name:lower()
    if la ~= lb then return la < lb end
    return a.name < b.name
  end)
  return entries
end

local function local_entries(dir)
  local names, msg, errno = fs.list(dir)
  if not names then return nil, msg, mft.code_of(errno) end
  local entries = {}
  for _, n in ipairs(names) do
    if not mft.is_part(n) then
      local st = fs.stat(mft.local_join(dir, n))
      if st and (st.type == "file" or st.type == "directory") then
        entries[#entries + 1] = { name = n, kind = st.type == "file" and "f" or "d",
                                  size = st.type == "file" and st.size or 0, mtime = st.mtime }
      end
    end
  end
  return entries
end

local function parent_dir(path)
  local dir = mft.dirname(path)
  return dir == "." and "/" or dir
end

-- ---- a file pane ----

local Pane = {}
Pane.__index = Pane

function Pane.new(ui, parent, title, remote)
  local self = setmetatable({ ui = ui, remote = remote, entries = {} }, Pane)
  self.cwd = remote and "." or (os.getenv("HOME") or fs.getcwd() or "/")
  local box = gui.vbox(parent)
  self.box = box
  self.title = gui.label(box, title)
  local bar = gui.hbox(box)
  self.up = gui.button(bar, "Up"):on("clicked", function() self:go_up(); return true end)
  self.path = gui.label(bar, ""):stretch(1, 0)
  self.view = gui.table(box):stretch(1, 1)
  self.view:model {
    columns = 3,
    rows = function(p) return p == -1 and #self.entries or 0 end,
    child = function(_, index) return index end,
    cell = function(row, col)
      local e = self.entries[row]
      if not e then return "" end
      if col == 1 then return e.name .. (e.kind == "d" and "/" or "") end
      if col == 2 then return e.kind == "d" and "" or human_size(e.size) end
      return format_time(e.mtime)
    end,
    header = function(col) return ({ "Name", "Size", "Modified" })[col] end,
  }
  self.view:column(1, 220):column(2, 80):column(3, 130)
  self.view:on("activate", function(_, e) self:activate(e.row); return true end)
  self.view:on("drag_begin", function(_, e) ui:drag_begin(self, e.row); return true end)
  self.view:on("drag_motion", function(_, e) return ui:drag_motion(self, e) end)
  self.view:on("drop", function(_, e) ui:drop(self, e); return true end)
  self.view:on("drag_end", function() ui:drag_end(); return true end)
  local buttons = gui.hbox(box)
  self.buttons = {}
  for _, b in ipairs({ { "New folder", self.new_folder }, { "Rename", self.rename },
                       { "Delete", self.delete }, { "Refresh", self.refresh } }) do
    self.buttons[#self.buttons + 1] = gui.button(buttons, b[1]):on("clicked", function() b[2](self); return true end)
  end
  gui.label(buttons, ""):stretch(1, 0)
  if remote then
    self.copy = gui.button(buttons, "Download"):on("clicked", function() ui:download(); return true end)
  else
    self.copy = gui.button(buttons, "Upload"):on("clicked", function() ui:upload(); return true end)
  end
  self.buttons[#self.buttons + 1] = self.copy
  return self
end

function Pane:show(entries)
  self.entries = sort_entries(entries)
  self.view:value(-1)
  self.view:refresh()
  self.path:text(self:display_path())
  self.up:enabled(self:can_go_up())
end

function Pane:display_path()
  if not self.remote then return self.cwd end
  return self.cwd == "." and "/" or "/" .. self.cwd
end

function Pane:can_go_up()
  if self.remote then return self.ui.connected and self.cwd ~= "." end
  return self.cwd ~= "/"
end

function Pane:join(name)
  if self.remote then return mft.join(self.cwd, name) end
  return mft.local_join(self.cwd, name)
end

function Pane:selected()
  local e = self.entries[self.view:value()]
  return e and { e } or {}
end

function Pane:names()
  local names = {}
  for _, e in ipairs(self.entries) do names[e.name] = true end
  return names
end

function Pane:refresh()
  if self.remote then
    if not self.ui.connected then return self:show({}) end
    self.ui:request("list", self.cwd)
    return
  end
  local entries, msg, code = local_entries(self.cwd)
  if not entries then
    self.ui:report(msg, code)
    entries = {}
  end
  self:show(entries)
end

function Pane:open(path)
  self.cwd = path
  self:refresh()
end

function Pane:go_up()
  if not self:can_go_up() then return end
  self:open(self.remote and mft.dirname(self.cwd) or parent_dir(self.cwd))
end

function Pane:activate(row)
  local e = self.entries[row]
  if e and e.kind == "d" then self:open(self:join(e.name)) end
end

function Pane:new_folder(name)
  name = name or self.ui:ask_string("New folder", "Name of the new folder:", "")
  if not name or name == "" then return end
  local path = self:join(name)
  if self.remote then return self.ui:request("mkdir", path) end
  local ok, msg, errno = fs.mkdir(path)
  if not ok then self.ui:report(msg, mft.code_of(errno)) end
  self:refresh()
end

function Pane:rename(new)
  local sel = self:selected()
  if #sel ~= 1 then return self.ui:set_status("Select one item to rename.") end
  local old = sel[1].name
  new = new or self.ui:ask_string("Rename", "New name of " .. old .. ":", old)
  if not new or new == "" or new == old then return end
  local src, dst = self:join(old), self:join(new)
  if self.remote then return self.ui:request("rename", src, dst) end
  if fs.lstat(dst) then
    self.ui:report("File exists", "EEXIST")
  else
    local ok, msg, errno = os.rename(src, dst)
    if not ok then self.ui:report(msg, mft.code_of(errno)) end
  end
  self:refresh()
end

function Pane:delete(confirm)
  local sel = self:selected()
  if #sel == 0 then return end
  local what = #sel == 1 and sel[1].name or (#sel .. " items")
  if confirm ~= false and not self.ui:ask_yes_no("Delete", "Delete " .. what .. "? Folders are removed with their contents.") then
    return
  end
  if self.remote then
    local paths = {}
    for _, e in ipairs(sel) do paths[#paths + 1] = self:join(e.name) end
    return self.ui:request("delete", table.unpack(paths))
  end
  for _, e in ipairs(sel) do
    local ok, msg, code = mft.remove_tree(self:join(e.name))
    if not ok then self.ui:report(msg, code) end
  end
  self:refresh()
end

function Pane:set_enabled(on)
  for _, b in ipairs(self.buttons) do b:enabled(on) end
end

-- ---- the window ----

-- UI.new{ host, port, app } builds the window. app defaults to gui.app().
function UI.new(opts)
  opts = opts or {}
  local self = setmetatable({}, UI)
  self.app = opts.app or assert(gui.app())
  self.connected = false
  self.busy = false
  self.steps = {}
  self.stats = nil
  self.listing = {}
  local win = self.app:window(980, 680, "Transfer")
  self.win = win
  win:on("close", function() self:close(); return true end)

  local conn = gui.hbox(win)
  gui.label(conn, "Host")
  self.host = gui.textfield(conn, opts.host or mft.DEFAULT_HOST):hint(180, 0)
  gui.label(conn, "Port")
  self.port = gui.textfield(conn, tostring(opts.port or mft.CLIENT_PORT)):hint(70, 0)
  self.connect_button = gui.button(conn, "Connect"):on("clicked", function() self:toggle_connection(); return true end)
  gui.label(conn, ""):stretch(1, 0)

  local serve = gui.hbox(win)
  self.serving = gui.checkbox(serve, "Serve"):on("toggled", function() self:toggle_server(); return true end)
  self.serve_dir = gui.textfield(serve, os.getenv("HOME") or fs.getcwd() or "/"):hint(300, 0)
  gui.button(serve, "Choose"):on("clicked", function() self:choose_folder(); return true end)
  gui.label(serve, "Port")
  self.serve_port = gui.textfield(serve, tostring(opts.serve_port or mft.SERVE_PORT)):hint(70, 0)
  self.serve_status = gui.label(serve, "Not serving"):stretch(1, 0)

  local panes = gui.splitpane(win, false):stretch(1, 1)
  self.local_pane = Pane.new(self, panes, "This computer", false)
  self.remote_pane = Pane.new(self, panes, "Not connected", true)
  panes:position(490)
  self.remote_pane:set_enabled(false)

  self.list = gui.table(win):hint(0, 150)
  self.list:model {
    columns = 4,
    rows = function(p) return p == -1 and #self.steps or 0 end,
    child = function(_, index) return index end,
    cell = function(row, col)
      local s = self.steps[row]
      if not s then return "" end
      if col == 1 then return s.label end
      if col == 2 then return s.direction == "get" and "Download" or "Upload" end
      if col == 3 then return s.kind == "d" and "" or human_size(s.size) end
      return s.status
    end,
    header = function(col) return ({ "Name", "Direction", "Size", "Status" })[col] end,
  }
  self.list:column(1, 360):column(2, 90):column(3, 90):column(4, 220)
  local bar = gui.hbox(win)
  self.progress = gui.progress(bar):range(0, 1000):hint(240, 0)
  self.summary = gui.label(bar, "No transfers"):stretch(1, 0)
  self.cancel_button = gui.button(bar, "Cancel"):enabled(false)
  self.cancel_button:on("clicked", function() self:cancel_transfers(); return true end)
  self.status = gui.label(win, "Ready")

  self.client = assert(thread.spawn(HERE .. "client.lua"))
  self.client_watch = self.app:watch(self.client:fd(), "r", function() self:pump() end)
  self.local_pane:refresh()
  self.remote_pane:show({})
  return self
end

function UI:run()
  return self.app:run()
end

-- ---- dialogs, replaceable by the tests ----

function UI:ask_string(title, prompt, initial)
  return self.app:prompt(title, prompt, initial)
end

function UI:ask_yes_no(title, message)
  return self.app:dialog(title, message, { "No", "Yes" }) == 2
end

function UI:choose_folder()
  local folder = self.app:choose_folder("Choose folder", self.serve_dir:text())
  if folder then self.serve_dir:text(folder) end
end

-- trace appends a line to the file of the environment variable
-- TRANSFER_LOG, for the boot case gui_transfer.
function UI:trace(line)
  local path = os.getenv("TRANSFER_LOG")
  if not path then return end
  local f = io.open(path, "a")
  if f then
    f:write(line, "\n")
    f:close()
  end
end

function UI:set_status(text)
  self.status:text(text)
  self:trace(text)
end

function UI:report(msg, code)
  self:set_status(string.format("Error: %s (%s)", msg or "error", code or "EIO"))
end

-- ---- the client worker ----

function UI:request(...)
  local ok, msg = self.client:send(table.concat({ ... }, SEP), 1000)
  if not ok then self:set_status("Error: " .. tostring(msg)) end
end

-- pump handles the messages of the client worker that are waiting.
function UI:pump()
  while self.client do
    local msg, _, code = self.client:receive(0)
    if not msg then
      if code == sys.errno.EPIPE then self:client_ended() end
      return
    end
    self:handle(split(msg))
  end
end

function UI:client_ended()
  if self.client_watch then self.client_watch:remove() end
  self.client_watch = nil
  local _, err = self.client:join()
  self.client:close()
  self.client = nil
  if err then self:set_status("Error: " .. err) end
end

function UI:handle(f)
  local kind = f[1]
  if kind == "connected" then
    self.connected = true
    self.connect_button:text("Disconnect"):enabled(true)
    self.remote_pane.title:text(f[2] .. ":" .. f[3] .. "  " .. f[4])
    self.remote_pane:set_enabled(true)
    self.remote_pane:open(".")
    self:set_status("Connected to " .. f[2] .. ":" .. f[3])
  elseif kind == "disconnected" or kind == "lost" then
    self.connected = false
    self.connect_button:text("Connect"):enabled(true)
    self.remote_pane.title:text("Not connected")
    self.remote_pane.cwd = "."
    self.remote_pane:show({})
    self.remote_pane:set_enabled(false)
    if kind == "lost" then self:report(f[2], "ECONNRESET") else self:set_status("Disconnected") end
  elseif kind == "failed" then
    if f[2] == "connect" then self.connect_button:enabled(true) end
    self:report(f[3], f[4])
    if f[2] == "mkdir" or f[2] == "rename" or f[2] == "delete" then self.remote_pane:refresh() end
  elseif kind == "listing" then
    local lines = (self.listing[f[2]] or "") .. f[4]
    if f[3] == "+" then
      self.listing[f[2]] = lines
      return
    end
    self.listing[f[2]] = nil
    if f[2] ~= self.remote_pane.cwd then return end
    local entries = {}
    for line in lines:gmatch("[^\n]+") do
      local k, size, mtime, name = line:match("^([df]) (%d+) (%-?%d+) (%S+)$")
      name = name and mft.unescape(name)
      if name then
        entries[#entries + 1] = { kind = k, size = math.tointeger(tonumber(size)),
                                  mtime = math.tointeger(tonumber(mtime)), name = name }
      end
    end
    self.remote_pane:show(entries)
    self:trace(string.format("listed %d items in %s", #entries, self.remote_pane:display_path()))
  elseif kind == "done" then
    self.remote_pane:refresh()
  elseif kind == "planned" then
    self:confirm_and_run(f[2], f[3])
  elseif kind == "step" then
    self.steps[tonumber(f[2])] = { direction = f[3], kind = f[4], size = tonumber(f[5]), label = f[6], status = "Waiting" }
    self.list:refresh()
  elseif kind == "start" then
    self:set_row(tonumber(f[2]), "Copying")
  elseif kind == "progress" then
    self:on_progress(tonumber(f[2]), tonumber(f[3]), tonumber(f[4]))
  elseif kind == "finished" then
    self:on_step_done(tonumber(f[2]), tonumber(f[3]))
  elseif kind == "stepfailed" then
    self:set_row(tonumber(f[2]), f[3])
  elseif kind == "batch" then
    self:finish_batch(f[2] == "cancelled")
  end
end

-- ---- connection ----

function UI:toggle_connection()
  if self.connected then self:disconnect() else self:connect() end
end

function UI:connect(host, port)
  host = host or self.host:text():match("^%s*(.-)%s*$")
  port = port or math.tointeger(tonumber(self.port:text()))
  if not port then return self:set_status("The port is not a number.") end
  self.connect_button:enabled(false)
  self:set_status(string.format("Connecting to %s:%d", host, port))
  self:request("connect", host, tostring(port))
end

function UI:disconnect()
  self:request("cancel")
  self:request("disconnect")
end

-- ---- server ----

function UI:toggle_server()
  if self.serving:value() ~= 0 then self:start_server() else self:stop_server() end
end

function UI:start_server(folder, port)
  folder = folder or self.serve_dir:text()
  port = port or math.tointeger(tonumber(self.serve_port:text()))
  if not port then
    self.serving:value(0)
    return self:set_status("The port is not a number.")
  end
  local worker, msg = thread.spawn(HERE .. "server.lua", port .. "\n" .. folder)
  if not worker then
    self.serving:value(0)
    return self:set_status("Error: " .. tostring(msg))
  end
  self.server, self.server_root, self.clients = worker, folder, 0
  self.serving:value(1)
  self.server_watch = self.app:watch(worker:fd(), "r", function() self:pump_server() end)
end

function UI:pump_server()
  while self.server do
    local msg, _, code = self.server:receive(0)
    if not msg then
      if code == sys.errno.EPIPE then self:server_ended() end
      return
    end
    local kind, rest = msg:match("^(%S+) ?(.*)$")
    if kind == "listening" then
      self.server_port = math.tointeger(tonumber(rest))
      self.serve_port:text(rest)
      self:set_status(string.format("Serving %s on port %s", self.server_root, rest))
      self:update_server_status()
    elseif kind == "failed" then
      self:set_status("Error: " .. rest)
    elseif kind == "log" then
      self:set_status(rest)
    elseif kind == "clients" then
      self.clients = math.tointeger(tonumber(rest)) or 0
      self:update_server_status()
    end
  end
end

function UI:server_ended()
  if self.server_watch then self.server_watch:remove() end
  self.server_watch = nil
  local _, err = self.server:join()
  self.server:close()
  self.server, self.server_port = nil, nil
  self.serving:value(0)
  if err then self:set_status("Error: " .. err) end
  self:update_server_status()
end

function UI:stop_server()
  if self.server then
    self.server:stop()
    self:server_ended()
  end
  self.serving:value(0)
  self:set_status("The server is stopped.")
end

function UI:update_server_status()
  if self.server and self.server_port then
    local n = self.clients or 0
    self.serve_status:text(string.format("Port %d, %d client%s connected", self.server_port, n, n == 1 and "" or "s"))
  else
    self.serve_status:text("Not serving")
  end
end

-- ---- transfers ----

-- upload(paths, target) copies local paths into the remote folder target.
function UI:upload(paths, target)
  if not self.connected then return self:set_status("Connect to a server first.") end
  if not paths then
    paths = {}
    for _, e in ipairs(self.local_pane:selected()) do paths[#paths + 1] = self.local_pane:join(e.name) end
  end
  if #paths == 0 then return end
  if self.busy then return self:set_status("A transfer is running.") end
  self:request("upload", target or self.remote_pane.cwd, table.unpack(paths))
end

-- download(entries, target) copies remote entries { kind, size, mtime,
-- path } into the local folder target.
function UI:download(entries, target)
  if not self.connected then return self:set_status("Connect to a server first.") end
  if not entries then
    entries = {}
    for _, e in ipairs(self.remote_pane:selected()) do
      entries[#entries + 1] = { kind = e.kind, size = e.size, mtime = e.mtime, path = self.remote_pane:join(e.name) }
    end
  end
  if #entries == 0 then return end
  if self.busy then return self:set_status("A transfer is running.") end
  local fields = {}
  for _, e in ipairs(entries) do
    fields[#fields + 1] = string.format("%s %d %d %s", e.kind, e.size, e.mtime, mft.encode(e.path))
  end
  self:request("download", target or self.local_pane.cwd, table.unpack(fields))
end

function UI:confirm_and_run(id, conflicts)
  if conflicts ~= "" then
    local list = {}
    for n in conflicts:gmatch("[^\n]+") do list[#list + 1] = n end
    local what = #list == 1 and list[1] or (#list .. " items")
    if not self:ask_yes_no("Replace", "The target folder already contains " .. what .. ". Replace?") then
      self:request("discard", id)
      return self:set_status("The copy was not started.")
    end
  end
  self.busy = true
  self.steps = {}
  self.list:refresh()
  self.stats = { files_done = 0, bytes_done = 0, moved = 0, current = 0, start = sys.clock_ns() }
  self.cancel_button:enabled(true)
  self:request("run", id)
end

function UI:set_row(i, status)
  local s = self.steps[i]
  if not s then return end
  s.status = status
  self.list:refresh()
  self:update_summary()
end

function UI:on_progress(i, done, size)
  local s = self.steps[i]
  if s then s.status = string.format("%d%%", size > 0 and done * 100 // size or 100) end
  self.progress:value(size > 0 and 1000 * done // size or 1000)
  self.stats.current = done
  self.list:refresh()
  self:update_summary()
end

function UI:on_step_done(i, moved)
  local s = self.steps[i]
  if not s then return end
  local resumed = s.kind == "f" and moved < s.size
  s.status = resumed and ("Done, resumed at " .. human_size(s.size - moved)) or "Done"
  if s.kind == "f" then
    self.stats.files_done = self.stats.files_done + 1
    self.stats.bytes_done = self.stats.bytes_done + s.size
    self.stats.moved = self.stats.moved + moved
  end
  self.stats.current = 0
  self.progress:value(1000)
  self.list:refresh()
  self:update_summary()
end

function UI:update_summary()
  local s = self.stats
  if not s then return self.summary:text("No transfers") end
  local files, bytes = 0, 0
  for _, step in ipairs(self.steps) do
    if step.kind == "f" then
      files = files + 1
      bytes = bytes + step.size
    end
  end
  local done = s.bytes_done + s.current
  local elapsed = math.max((sys.clock_ns() - s.start) / 1e9, 1e-3)
  local rate = (s.moved + s.current) / elapsed
  self.summary:text(string.format("%d of %d files, %s of %s, %s/s", s.files_done, files, human_size(done),
                                  human_size(bytes), human_size(math.floor(rate))))
end

function UI:finish_batch(cancelled)
  self.busy = false
  self.cancel_button:enabled(false)
  self.local_pane:refresh()
  self.remote_pane:refresh()
  self:update_summary()
  if cancelled then
    self:set_status("Cancelled")
  else
    local files = 0
    for _, step in ipairs(self.steps) do
      if step.kind == "f" then files = files + 1 end
    end
    self:set_status(string.format("Copied %d of %d files", self.stats.files_done, files))
  end
end

function UI:cancel_transfers()
  self:request("cancel")
  self:set_status("Cancelling")
end

-- ---- drag and drop ----

-- Rows of the local pane are dragged as text/uri-list, so that Files and
-- the desktop accept them as well. Rows of the remote pane are dragged as
-- ENTRY_MIME: the line "HOST PORT" of the session and the line "KIND SIZE
-- MTIME PATH" of the entry.
function UI:drag_begin(pane, row)
  local e = pane.entries[row]
  if not e then return end
  pane.view:selectrow(row)
  local items
  if pane.remote then
    if not self.connected then return end
    items = { { ENTRY_MIME, string.format("%s\n%s %d %d %s\n", self:session_key(), e.kind, e.size, e.mtime,
                                          mft.encode(pane:join(e.name))) } }
  else
    local path = pane:join(e.name)
    items = { { "text/uri-list", mft.file_uri(path) .. "\r\n" }, { "text/plain", path } }
  end
  self.drag_source = pane
  pane.view:drag(items, gui.DND_COPY, e.name)
  self:set_status("Dragging 1 item")
end

function UI:drag_end()
  if self.drag_source then
    self.drag_source = nil
    self:set_status("Ready")
  end
end

function UI:session_key()
  return self.host:text() .. ":" .. self.port:text()
end

-- The folder of a drop on pane at row: a folder row, or the folder of the
-- pane. The row -1 outlines the whole view.
local function drop_folder(pane, row)
  local e = pane.entries[row]
  if e and e.kind == "d" then return pane:join(e.name), row end
  return pane.cwd, -1
end

function UI:drag_motion(pane, ev)
  local mime
  if pane.remote then
    if self.connected and gui.offers("text/uri-list") then mime = "text/uri-list" end
  elseif gui.offers(ENTRY_MIME) then
    mime = ENTRY_MIME
  end
  if not mime then return false end
  local _, row = drop_folder(pane, ev.row)
  ev.accept, ev.actions, ev.preferred, ev.row = mime, gui.DND_COPY, gui.DND_COPY, row
  return true
end

function UI:drop(pane, ev)
  self.drag_source = nil
  local folder = drop_folder(pane, ev.row)
  if pane.remote and ev.mime == "text/uri-list" then
    local paths = mft.uri_paths(ev.data or "")
    if #paths > 0 then self:upload(paths, folder) end
  elseif not pane.remote and ev.mime == ENTRY_MIME then
    local key, rest = (ev.data or ""):match("^([^\n]*)\n(.*)$")
    if key ~= self:session_key() then return self:set_status("The items belong to another session.") end
    local entries = {}
    for line in rest:gmatch("[^\n]+") do
      local k, size, mtime, path = line:match("^([df]) (%d+) (%-?%d+) (%S+)$")
      path = path and mft.unescape(path)
      if path then
        entries[#entries + 1] = { kind = k, size = math.tointeger(tonumber(size)),
                                  mtime = math.tointeger(tonumber(mtime)), path = path }
      end
    end
    if #entries > 0 then self:download(entries, folder) end
  end
end

-- ---- closing ----

function UI:close()
  if self.closed then return end
  self.closed = true
  if self.server then
    self.server:stop()
    self:server_ended()
  end
  if self.client then
    if self.client_watch then self.client_watch:remove() end
    self.client_watch = nil
    self.client:close()
    self.client = nil
  end
  self.app:quit(0)
end

UI.Pane = Pane
UI.sort_entries = sort_entries
return UI
