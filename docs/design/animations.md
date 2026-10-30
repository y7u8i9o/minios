# Animations of X12

X12 animates windows and popups on the CPU (milestones AN1 to AN4 of
`docs/plan/animation-gpu.md`). The animations follow the practice of
mature compositors such as Mutter, KWin and the CSS and Material motion
specifications:

- An animation changes visual parameters only: the opacity, a scale
  around the centre and an offset. The geometry, the stacking order, the
  keyboard focus and the input regions take their final values at once.
  A click during an animation acts on the final state.
- The progress depends on the time since the start. A late frame shows a
  later state. The duration therefore does not depend on the frame rate.
- A new animation of a surface starts from the values on the screen. An
  interrupted animation therefore never jumps.
- Entering elements decelerate, leaving elements accelerate, and an
  element that moves between two places on the screen uses the standard
  curve.
- One setting scales every duration. The value 0 disables the animations
  completely.

## Animation clock (AN1)

`user/compositor/anim.c` contains the clock. Every surface has visual
parameters (`struct visual` in `comp.h`) and one animation (`struct
anim`):

- `alpha`: 0 to 255, multiplied into the alpha of every pixel.
- `scale`: 16.16 fixed point. The scene scales the extent of the surface
  (`wm_extent`: the surface, or the frame with the shadow of a server
  decorated window) around the centre of the extent.
- `dx`, `dy`: an offset in device pixels after the scale.

`VISUAL_IDENTITY` (alpha 255, scale 1, no offset) changes nothing. A new
surface has the identity. `anim_start(s, from, to, ms, curve)` starts an
animation of `s` from `from`, or from the current values for `NULL`, to
`to`. The duration is `ms` multiplied by the setting `anim_percent` and
divided by 100.

The frame clock of `main.c` treats a running animation as pending work
(`schedule_frame`). Each frame first calls `anim_advance` with the start
time of the frame. The advance computes the linear progress from the
time since the start of each animation, applies the easing curve and
mixes the visual parameters. Each step damages the transformed extent of
the surface before and after the step (`scene_visual_extent`). The last
step sets the final values exactly and ends the animation. Without a
running animation the frame clock idles as before (G6 of
`docs/plan/compositor-performance.md`).

### Easing curves

The curves are fixed point with 16 fractional bits, as the pixel code.
The kernel and X12 use no floating point in these paths.

| Curve | Definition | Use |
|---|---|---|
| `ANIM_EASE_OUT_CUBIC` | 1 - (1 - t)^3 | an element enters: a window maps, a popup opens |
| `ANIM_ACCELERATE` | cubic-bezier(0.4, 0, 1, 1) | an element leaves: a window closes, a popup closes |
| `ANIM_STANDARD` | cubic-bezier(0.2, 0, 0, 1) | an element moves on the screen: minimize and restore |

The Bézier curves have the CSS form with the end points (0, 0) and
(1, 1). The x coordinate of such a curve grows monotonically, so
seventeen bisection steps find the curve parameter of a time to the last
bit of 16.16. The y coordinate at that parameter is the eased progress.
Every product in `bezier_at` is shifted back to 16.16 at once and fits
into 64 bits.

### Setting anim_percent

The settings interface (`debug.c`) has the key `anim_percent`: 0 to 400,
100 by default. 100 is the normal speed. 400 shows every animation four
times slower for inspection. 0 disables the animations: a started
animation sets its final values at once, and the change to 0 ends every
running animation. The Settings page of x12settings has the control
"Animation duration (%)". `compstat` prints the keys `anim_percent`,
`animations` (running now) and `anim_frames` (frames that advanced an
animation since the last reset, `graphics-performance.md`).

X12 is single threaded. The animations are part of the surfaces and
share the state of the scene without a lock.

### Tests

The boot tests start X12 through `start_x12` of
`kernel/tests/gui_helpers.h`. The helper runs `x12settings set
anim_percent 0` after X12 accepts clients, so every pixel check sees the
final state of a map, a close or a minimize at once. x12settings is the
first client of X12 in such a test, and the first client of the test
itself has the number 2. The cases that start X12 through the greeter run
with the default speed. Their checks wait for the expected pixels with a
timeout.
