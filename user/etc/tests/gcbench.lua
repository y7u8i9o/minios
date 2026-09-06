-- Allocation benchmark of the Lua port: n tables with a string each,
-- then a full collection. With the first fit allocator 40000 objects took
-- 63 seconds to allocate under TCG; the case times out at 60 seconds so
-- a return to quadratic behaviour fails it.
for _, n in ipairs{5000, 10000, 20000, 40000} do
  collectgarbage("collect")
  local t0 = os.clock()
  local waste = {}
  for i = 1, n do waste[i] = {i, tostring(i)} end
  local t1 = os.clock()
  waste = nil
  collectgarbage("collect")
  local t2 = os.clock()
  print(string.format("n=%d alloc %.3fs collect %.3fs", n, t1 - t0, t2 - t1))
end
