-- GUI-side model. Mutable DSP state is confined to worker.lua.
local thread, sys = require 'thread', require 'sys'
local root = debug.getinfo(1, 'S').source:sub(2):match('^(.*[/])') or './'
local M, Control = {}, {}
Control.__index = Control
function M.new(synth)
  local params = assert(synth.patch(synth.presets[2]))
  local worker = assert(thread.spawn(root .. 'worker.lua', assert(synth.encode(params))))
  return setmetatable({remote=true, synth=synth, params=params, worker=worker,
    voices=0, peak=0, scope={}, xruns=0, stats={}, ready=false}, Control)
end
function Control:send(message)
  if self.error then return nil, self.error end
  local ok, err = self.worker:send(message)
  if not ok then
    self.error = 'audio control channel: ' .. tostring(err)
    self.worker:stop() -- Fail silent instead of dropping a note-off command.
  end
  return ok, self.error
end
function Control:fd() return self.worker:fd() end
function Control:active() return self.voices end
function Control:note_on(note, velocity) return self:send(string.format('on %d %d', note, velocity or 100)) end
function Control:note_off(note) return self:send('off ' .. note) end
function Control:set_pedal(down)
  self.pedal = not not down
  return self:send(down and 'pedal 1' or 'pedal 0')
end
function Control:all_off()
  self.pedal = false
  return self:send('all_off')
end
function Control:panic()
  self.voices, self.peak, self.scope = 0, 0, {}
  return self:send('panic')
end
function Control:set(key, value)
  local patch = {}
  for k, v in pairs(self.params) do patch[k] = v end
  patch[key] = value
  local valid, err = self.synth.patch(patch)
  if not valid then return nil, err end
  local known = false
  for _, c in ipairs(self.synth.controls) do if c[1] == key then known = true end end
  if not known then return nil, 'unknown control' end
  local ok
  ok, err = self:send(string.format('set %s %d', key, value))
  if ok then self.params = valid end
  return ok, err
end
function Control:apply(patch)
  local valid, err = self.synth.patch(patch)
  if not valid then return nil, err end
  local ok
  ok, err = self:send('patch\n' .. assert(self.synth.encode(valid)))
  if ok then self.params, self.voices, self.peak, self.scope = valid, 0, 0, {} end
  return ok, err
end
function Control:dispatch()
  for _ = 1, 128 do
    local packet, _, code = self.worker:receive()
    if not packet then
      if code ~= sys.errno.ETIMEDOUT and code ~= sys.errno.EPIPE then self.error = 'audio message receive failed' end
      break
    end
    local kind = packet:sub(1, 1)
    if kind == 'R' then
      self.ready, self.thread_id = true, tonumber(packet:sub(2))
    elseif kind == 'N' then
      local note, voices = packet:sub(2):match('^(%d+) (%d+)$')
      self.voices = tonumber(voices)
      if self.on_note then self.on_note(tonumber(note), self.voices) end
    elseif kind == 'B' then
      self.ack = tonumber(packet:sub(2))
    elseif kind == 'M' then
      local render, maximum, cpu, tid, frames, pos
      self.voices, self.xruns, self.peak, render, maximum, cpu, tid, frames, pos =
        string.unpack('<I4I4fI8I8I8I8I8', packet, 2)
      self.stats = {render_ns=render, render_max_ns=maximum, cpu_ns=cpu, thread_id=tid, frames=frames}
      self.scope = {}
      while pos <= #packet do
        local sample
        sample, pos = string.unpack('<f', packet, pos)
        self.scope[#self.scope + 1] = sample
      end
    end
  end
  if self.worker:status() ~= 'running' then
    local ok, err = self.worker:join()
    if not ok then self.error = err elseif not self.closing then self.error = 'audio worker exited' end
  end
  return not self.error, self.error
end
-- A command barrier for tests and operations requiring an applied patch.
-- Normal keyboard and slider handlers remain asynchronous.
function Control:sync(timeout)
  self.sequence = (self.sequence or 0) + 1
  local sequence = self.sequence
  local ok, err = self:send('barrier ' .. sequence)
  if not ok then return nil, err end
  local deadline = sys.uptime() + (timeout or 2000)
  local fds = {{fd=self:fd(), events=sys.POLLIN}}
  repeat
    ok, err = self:dispatch()
    if not ok then return nil, err end
    if self.ack == sequence then return true end
    ok, err = sys.poll(fds, 20)
    if not ok then return nil, err end
  until sys.uptime() >= deadline
  return nil, 'audio command barrier timed out'
end
function Control:close()
  if self.closed then return end
  self.closing = true
  self.worker:stop()
  local ok, err = self.worker:join()
  self:dispatch()
  self.worker:close()
  self.closed = true
  if not ok then self.error = err end
  return ok, err
end
return M
