local directory = (arg[0]:match("^(.*[/])") or "./")
local synth = dofile(directory .. "engine.lua")
local ui = dofile(directory .. "ui.lua")
local view = ui.new(synth)
if arg[1] then
  local ok, err = view:load(arg[1])
  if not ok then view.message = "Load failed: " .. tostring(err) end
end
os.exit(view:run())
