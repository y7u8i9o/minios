-- This script is the second half of the lua_prompt boot test. It checks
-- that the interactive session before it wrote $HOME/.lua_history at
-- exit, with the completed and the recalled lines and without the line
-- discarded with Ctrl+C, and that the module directory of the share
-- tree exists and is on the search path of require.
local fs = require "fs"

local f = assert(io.open(os.getenv("HOME") .. "/.lua_history"))
local history = f:read("a")
f:close()
local function has(line) return history:find(line .. "\n", 1, true) ~= nil end
assert(has('print("up " .. string.upper ("ok"))'), "the completed line is missing from the history")
assert(has('n = (n or 0) + 1; print("run " .. n)'), "the counter line is missing from the history")
assert(not history:find("discarded", 1, true), "the discarded line is in the history")
print("luaprompt: the history was saved")

local st = fs.stat("/usr/share/lua/5.5")
assert(st and st.type == "directory", "the module directory does not exist")
assert(package.path:find("/usr/share/lua/5.5/?.lua", 1, true), "require does not search the module directory")
print("luaprompt: the module directory exists")
