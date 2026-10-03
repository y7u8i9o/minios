-- pong: the left paddle follows w and s, the right paddle the arrow
-- keys. First to 7 points wins. Escape quits. A 16 ms timer drives the
-- game; the canvas draws the field.
local gui = require "gui"

local PAD_H, PAD_W, BALL = 60, 8, 8
local W, H = 480, 320
local ly, ry = H // 2 - PAD_H // 2, H // 2 - PAD_H // 2
local bx, by, vx, vy = 0, 0, 0, 0
local lscore, rscore = 0, 0
local pressed = {}
local winner_ticks = 0

local function reset_ball(dir)
  bx, by = W // 2 - BALL // 2, H // 2 - BALL // 2
  vx = 3 * dir
  vy = math.random(-2, 2)
  if vy == 0 then vy = 1 end
end

local app = assert(gui.app())
local win = app:window(W, H, "pong"):padding(0)
local canvas = gui.canvas(win)

canvas:on("paint", function(w, p)
  W, H = w:size()
  p:fill(0, 0, W, H, 0x101820)
  for y = 0, H - 1, 16 do p:fill(W // 2 - 1, y, 2, 8, 0x405060) end
  p:fill(10, ly, PAD_W, PAD_H, 0xe0e0e0)
  p:fill(W - 10 - PAD_W, ry, PAD_W, PAD_H, 0xe0e0e0)
  p:fill(bx, by, BALL, BALL, 0xffd040)
  p:text(W // 2 - 40, 8, tostring(lscore), 0xe0e0e0)
  p:text(W // 2 + 32, 8, tostring(rscore), 0xe0e0e0)
  if winner_ticks > 0 then
    p:text(W // 2 - 60, H // 2 - 8, lscore == 7 and "left player wins" or "right player wins", 0xffffff)
  end
end)

local function step()
  if winner_ticks > 0 then
    winner_ticks = winner_ticks - 1
    if winner_ticks == 0 then lscore, rscore = 0, 0 end
    canvas:invalidate()
    return
  end
  if pressed[gui.key.w] and ly > 0 then ly = ly - 4 end
  if pressed[gui.key.s] and ly < H - PAD_H then ly = ly + 4 end
  if pressed[gui.key.up] and ry > 0 then ry = ry - 4 end
  if pressed[gui.key.down] and ry < H - PAD_H then ry = ry + 4 end
  bx, by = bx + vx, by + vy
  if by <= 0 or by + BALL >= H then vy = -vy end
  if bx <= 10 + PAD_W and by + BALL >= ly and by <= ly + PAD_H then vx = -vx; bx = 10 + PAD_W end
  if bx + BALL >= W - 10 - PAD_W and by + BALL >= ry and by <= ry + PAD_H then vx = -vx; bx = W - 10 - PAD_W - BALL end
  if bx < 0 then rscore = rscore + 1; reset_ball(1) end
  if bx > W then lscore = lscore + 1; reset_ball(-1) end
  if lscore == 7 or rscore == 7 then winner_ticks = 120 end
  canvas:invalidate()
end

canvas:on("key", function(w, k)
  if k.code == gui.key.esc then app:quit(0) end
  pressed[k.code] = true
  return true
end)
canvas:on("keyup", function(w, k) pressed[k.code] = nil; return true end)
canvas:focus()
reset_ball(1)
app:timer(16, true, step)
app:run()
