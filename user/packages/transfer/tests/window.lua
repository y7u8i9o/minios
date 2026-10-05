-- Host test of the window of Transfer (ui.lua) against the server of
-- tools/transfer.py (make check-transfer). The test drives the window
-- through its methods and through the injected mouse and drag messages
-- of gui.test, in a scratch folder given as the first argument.
local WORK = assert(arg[1], "usage: window.lua WORKDIR")
if WORK:sub(1, 1) ~= "/" then WORK = require("fs").getcwd() .. "/" .. WORK end
local FILES = "user/packages/transfer/files/"
package.path = FILES .. "usr/share/lua/5.5/?.lua;" .. package.path
local gui = require "gui"
local fs = require "fs"
local sys = require "sys"
local mft = require "mft"
local UI = dofile(FILES .. "usr/share/apps/transfer/ui.lua")

local checks = 0
local function check(v, why)
  if not v then error("FAIL " .. why, 2) end
  checks = checks + 1
end

local function sh(cmd)
  local ok = os.execute(cmd)
  return ok == true or ok == 0
end
local function q(s) return "'" .. s:gsub("'", "'\\''") .. "'" end
local function write(path, data)
  local f = assert(io.open(path, "wb"))
  f:write(data)
  f:close()
end
local function read(path)
  local f = io.open(path, "rb")
  if not f then return nil end
  local d = f:read("a")
  f:close()
  return d
end

sh("rm -rf " .. q(WORK))
assert(fs.mkdir(WORK))
local A, L, S = WORK .. "/py-root", WORK .. "/local", WORK .. "/served"
for _, d in ipairs({ A, L, S, A .. "/tree", A .. "/tree/sub", L .. "/folder" }) do assert(fs.mkdir(d)) end
write(A .. "/remote.txt", "remote file\n")
write(A .. "/tree/sub/leaf.txt", "leaf\n")
write(L .. "/up.txt", "uploaded by the window\n")
write(L .. "/dragged.txt", "dragged into the remote pane\n")
write(L .. "/folder/inner.txt", "inner\n")
write(L .. "/" .. mft.part_name("hidden", 10, 10), "part")
write(S .. "/served.txt", "served by the window\n")

local server = assert(io.popen("sh -c 'echo $$; exec python3 tools/transfer.py serve -p 0 " .. q(A) .. "'"))
local pypid = math.tointeger(tonumber(server:read("l")))
local pyport = math.tointeger(tonumber((server:read("l") or ""):match("on port (%d+)$")))
check(pypid and pyport, "python server started")
-- The body runs under xpcall. The server is stopped after a failure as
-- well, because closing the pipe of io.popen waits for its process.
local function body()

  local ui = UI.new { host = "127.0.0.1", port = pyport }
  local answers = {}
  function ui:ask_yes_no(title, message)
    answers[#answers + 1] = message
    return answers.reply
  end
  function ui:ask_string() return answers.name end

  local function pump_until(cond, ms)
    local deadline = sys.uptime() + (ms or 5000)
    while not cond() and sys.uptime() < deadline do ui.app:step(20) end
    return cond()
  end

  local function row_of(pane, name)
    for i, e in ipairs(pane.entries) do
      if e.name == name then return i end
    end
  end

  local function idle()
    return not ui.busy
  end

  -- ---- the local pane and the connection ----

  ui.local_pane:open(L)
  check(row_of(ui.local_pane, "up.txt") and row_of(ui.local_pane, "folder"), "local listing")
  check(not row_of(ui.local_pane, "hidden") and #ui.local_pane.entries == 3, "part files hidden")
  check(ui.local_pane.entries[1].name == "folder", "folders first")
  check(ui.local_pane.path:text() == L, "local path")
  check(ui.status:text() == "Ready", "ready")
  ui:upload()
  check(ui.status:text() == "Connect to a server first.", "upload needs a connection")
  ui:connect()
  check(pump_until(function() return ui.connected and #ui.remote_pane.entries == 2 end), "connected and listed")
  check(ui.remote_pane.title:text() == "127.0.0.1:" .. pyport .. "  py-root", "remote title")
  check(ui.connect_button:text() == "Disconnect", "disconnect button")
  check(ui.status:text() == "Connected to 127.0.0.1:" .. pyport, "connected status")
  check(ui.remote_pane.path:text() == "/", "remote root")

  -- ---- an upload of the selection ----

  ui.local_pane.view:selectrow(row_of(ui.local_pane, "up.txt"))
  ui:upload()
  check(pump_until(function() return ui.stats and idle() end), "upload finished")
  check(read(A .. "/up.txt") == "uploaded by the window\n", "uploaded")
  check(ui.steps[1].status == "Done" and ui.steps[1].direction == "put", "transfer row")
  check(ui.status:text() == "Copied 1 of 1 files", "copied status: " .. ui.status:text())
  check(ui.summary:text():match("^1 of 1 files, 23 B of 23 B, "), "summary: " .. ui.summary:text())
  check(pump_until(function() return row_of(ui.remote_pane, "up.txt") end), "remote pane refreshed")

  -- the same upload again asks before it replaces the file
  answers.reply = false
  ui.local_pane.view:selectrow(row_of(ui.local_pane, "up.txt"))
  ui:upload()
  check(pump_until(function() return #answers == 1 end), "replace question")
  check(answers[1] == "The target folder already contains up.txt. Replace?", "replace text")
  check(ui.status:text() == "The copy was not started.", "not started")
  answers.reply = true
  ui.local_pane.view:selectrow(row_of(ui.local_pane, "folder"))
  ui:upload()
  check(pump_until(function() return ui.stats and idle() and read(A .. "/folder/inner.txt") end), "folder uploaded")

  -- ---- a download of a folder ----

  ui.remote_pane.view:selectrow(row_of(ui.remote_pane, "tree"))
  ui:download()
  check(pump_until(function() return idle() and read(L .. "/tree/sub/leaf.txt") end), "folder downloaded")
  check(#ui.steps == 3 and ui.steps[1].label == "tree" and ui.steps[3].label == "tree/sub/leaf.txt", "download rows")
  check(row_of(ui.local_pane, "tree"), "local pane refreshed")

  -- ---- remote file management ----

  answers.name = "made"
  ui.remote_pane:new_folder()
  check(pump_until(function() return row_of(ui.remote_pane, "made") end), "remote new folder")
  ui.remote_pane.view:selectrow(row_of(ui.remote_pane, "made"))
  answers.name = "renamed"
  ui.remote_pane:rename()
  check(pump_until(function() return row_of(ui.remote_pane, "renamed") end), "remote rename")
  ui.remote_pane.view:selectrow(row_of(ui.remote_pane, "renamed"))
  ui.remote_pane:delete()
  check(pump_until(function() return not row_of(ui.remote_pane, "renamed") end), "remote delete")
  check(not fs.exists(A .. "/renamed"), "remote folder removed")
  ui.remote_pane:activate(row_of(ui.remote_pane, "tree"))
  check(pump_until(function() return ui.remote_pane.path:text() == "/tree" and row_of(ui.remote_pane, "sub") end),
        "open a remote folder")
  ui.remote_pane:go_up()
  check(pump_until(function() return ui.remote_pane.path:text() == "/" and row_of(ui.remote_pane, "tree") end), "up")

  -- ---- local file management ----

  answers.name = "local made"
  ui.local_pane:new_folder()
  check(fs.stat(L .. "/local made"), "local new folder")
  ui.local_pane.view:selectrow(row_of(ui.local_pane, "local made"))
  ui.local_pane:delete()
  check(not fs.exists(L .. "/local made"), "local delete")

  -- ---- drag and drop ----

  gui.test.paint(ui.win)
  -- The centre of the row of name in a pane, in window coordinates.
  local function row_point(pane, name)
    local x, y = pane.view:abs()
    local rx, ry, rw, rh = pane.view:rowrect(row_of(pane, name))
    assert(rx, name .. " is not visible")
    return x + rx + 30, y + ry + rh // 2
  end

  -- A drop of a file of Files on the empty part of the remote pane uploads it.
  local rx, ry = ui.remote_pane.view:abs()
  local rw, rh = ui.remote_pane.view:size()
  local mime, actions, preferred = gui.test.drag(ui.win, rx + 20, ry + rh - 6, { "text/uri-list", "text/plain" })
  check(mime == "text/uri-list" and actions == gui.DND_COPY and preferred == gui.DND_COPY, "remote pane accepts files")
  gui.test.drop(ui.win, rx + 20, ry + rh - 6, "text/uri-list", mft.file_uri(L .. "/dragged.txt") .. "\r\n", gui.DND_COPY)
  check(pump_until(function() return idle() and read(A .. "/dragged.txt") end), "dropped file uploaded")
  check(pump_until(function() return row_of(ui.remote_pane, "dragged.txt") end), "remote pane shows the dropped file")
  -- The local pane refuses files of Files and accepts remote entries.
  local lx, ly = ui.local_pane.view:abs()
  local _, lh = ui.local_pane.view:size()
  check(gui.test.drag(ui.win, lx + 20, ly + lh - 6, { "text/uri-list" }) == nil, "local pane refuses files")
  gui.test.drop(ui.win, lx + 20, ly + lh - 6, "text/uri-list", "", gui.DND_COPY)

  -- A press on a row of the remote pane and a move start a drag.
  local px, py = row_point(ui.remote_pane, "remote.txt")
  local started = gui.test.dragged()
  gui.test.mouse(ui.win, "down", px, py, 1)
  gui.test.mouse(ui.win, "move", px + 30, py + 10, 1)
  local count, items = gui.test.dragged()
  check(count == started + 1 and items[1][1] == "application/x-mft-entry", "remote drag started")
  check(items[1][2]:match("^127.0.0.1:" .. pyport .. "\nf 12 %-?%d+ remote.txt\n$"), "remote drag data: " .. items[1][2])
  check(ui.status:text() == "Dragging 1 item", "dragging status")
  -- The drop of that entry on the folder row of the local pane downloads into the folder.
  mime = gui.test.drag(ui.win, lx + 20, ly + lh - 6, { "application/x-mft-entry" })
  check(mime == "application/x-mft-entry", "local pane accepts remote entries")
  local fx, fy = row_point(ui.local_pane, "folder")
  gui.test.drag(ui.win, fx, fy, { "application/x-mft-entry" })
  gui.test.drop(ui.win, fx, fy, "application/x-mft-entry", items[1][2], gui.DND_COPY)
  check(pump_until(function() return idle() and read(L .. "/folder/remote.txt") end), "drop on a folder row")
  gui.test.drag_end(ui.win, gui.DND_COPY)
  -- Entries of another session are refused.
  gui.test.drag(ui.win, lx + 20, ly + lh - 6, { "application/x-mft-entry" })
  gui.test.drop(ui.win, lx + 20, ly + lh - 6, "application/x-mft-entry", "other:1\nf 1 1 x\n", gui.DND_COPY)
  check(ui.status:text() == "The items belong to another session.", "foreign entries refused")
  -- A press and a move on a local row drag a file URI.
  local ux, uy = row_point(ui.local_pane, "up.txt")
  gui.test.mouse(ui.win, "down", ux, uy, 1)
  gui.test.mouse(ui.win, "move", ux + 30, uy + 10, 1)
  count, items = gui.test.dragged()
  check(items[1][1] == "text/uri-list" and items[1][2] == mft.file_uri(L .. "/up.txt") .. "\r\n", "local drag uri")
  check(items[2][1] == "text/plain" and items[2][2] == L .. "/up.txt", "local drag path")
  gui.test.drag_end(ui.win, 0)
  check(ui.status:text() == "Ready", "drag end")

  -- ---- the server of the window ----

  ui.serve_dir:text(S)
  ui.serve_port:text("0")
  ui.serving:value(1)
  ui:toggle_server()
  check(pump_until(function() return ui.server_port end), "window server listening")
  check(ui.serve_status:text() == "Port " .. ui.server_port .. ", 0 clients connected", "server status")
  local out = WORK .. "/out"
  assert(fs.mkdir(out))
  local conn = assert(mft.connect("127.0.0.1", ui.server_port))
  check(pump_until(function() return ui.clients == 1 end), "client count")
  check(ui.serve_status:text() == "Port " .. ui.server_port .. ", 1 client connected", "server status with a client")
  local st = assert(conn:stat("served.txt"))
  check(conn:get("served.txt", st.size, st.mtime, out, "served.txt") == st.size, "get from the window server")
  conn:quit()
  check(pump_until(function() return ui.clients == 0 end), "client gone")
  check(pump_until(function() return ui.status:text() == "sent served.txt, 21 bytes" end), "server log line")
  ui.serving:value(0)
  ui:toggle_server()
  check(ui.serve_status:text() == "Not serving" and ui.status:text() == "The server is stopped.", "server stopped")

  -- ---- disconnection and closing ----

  ui:toggle_connection()
  check(pump_until(function() return not ui.connected end), "disconnected")
  check(ui.remote_pane.title:text() == "Not connected" and #ui.remote_pane.entries == 0, "remote pane cleared")
  check(ui.status:text() == "Disconnected", "disconnected status")
  ui:close()
  check(ui.client == nil and ui.server == nil, "workers closed")
end

local ok, err = xpcall(body, debug.traceback)
sys.kill(pypid, "TERM")
server:close()
if not ok then error(err, 0) end
sh("rm -rf " .. q(WORK))
print("transfer: " .. checks .. " window checks passed")
