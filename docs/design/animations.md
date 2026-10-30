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
anim_percent 0` after X12 accepts clients. Every pixel check therefore
sees the final state of a map, a close or a minimize at once. x12settings is the
first client of X12 in such a test, and the first client of the test
itself has the number 2. The cases that start X12 through the greeter run
with the default speed. Their checks wait for the expected pixels with a
timeout.

## Visual parameters in the blend path (AN2)

### Pixel functions

`lib/libgui/src/pixel.c` has three functions for the animations
(`gui/pixel.h`):

- `pixel_over_alpha(to, from, n, alpha)`: `pixel_over` with the alpha of
  each source pixel multiplied by `alpha / 255`. The vector form computes
  the product in the low 16 bit lane of each 32 bit lane and divides it
  by 255 as `pixel_div255`. A group of four transparent source pixels is
  skipped.
- `pixel_copy_alpha(to, from, n, alpha)`: the colour of each source pixel
  over the destination with the coverage `alpha`, the alpha byte of the
  source ignored.
- `pixel_sample_bilinear(to, from, stride, w, h, u, v, du, n, opaque)`:
  bilinear samples along a row for fractional scales. The positions are
  16.16, and the weights use the upper 8 fractional bits. Pixels outside
  the image are transparent. A scaled image therefore has antialiased
  edges.
  The colours are weighted with their alpha (premultiplied
  interpolation), and the result has straight alpha. A transparent
  neighbour therefore does not darken an edge. A position on a pixel
  centre gives the pixel exactly.

Both alpha functions have a vector and a word form, like `pixel_over`. On
x86_64 they use the form that the first call measures for `pixel_over`.
The host tests (`make check-libgui`, `lib/libgui/tests/test_pixel.c`)
compare every form with the scalar references of `pixel_impl.h` on rows
of every offset and length, check the end values 0 and 255 against
`pixel_over` and `pixel_copy_opaque`, and compare the bilinear sampler
with a reference in double precision within one step per channel. The
guest program `pixeltest` runs the same file with the code of the cross
compilers.

### Drawing

A surface with visual parameters other than the identity is drawn from
an image of its extent (`struct scene_image` in `comp.h`, `scene.c`):

- `render_extent` renders the extent with the identity in device pixels
  with straight alpha. An undecorated surface whose buffer has the
  screen scale is copied. An XRGB buffer and the opaque region of an
  ARGB buffer receive the alpha 255 in the copy, because clients may
  leave the alpha byte of opaque pixels at 0. Every other surface (a
  server decorated window, a buffer of another scale or a transform) is
  drawn twice, over black and over white. The difference of the two
  results gives the alpha of each pixel. The screen capture of a window
  (`scene_render_window`) uses the same function.
- `surface_image` renders the image again when the extent, the screen
  scale, the commits of the surface or the state of the decorations
  changed. A transformed surface therefore shows new contents during an
  animation.
- `draw_image` maps every destination pixel back into the image: the
  position c + (X + 1/2 - d - c) / scale - 1/2, with c the centre of the
  extent and d the offset. A scale of 1 at the screen scale of the image
  copies rows with `pixel_over_alpha`. Every other scale samples the
  rows with `pixel_sample_bilinear` in pieces of 256 pixels and blends
  them with `pixel_over_alpha`.

The server side decorations and the shadow are part of the image. The
plan intended `decor.c` to draw the shadow and the corner coverage with
the alpha and the scale of the window. The image draws the title, the
buttons and the shadow with one transform instead, and `decor.c` remains
unchanged. The dimming of an unresponsive client is left out while its
window is transformed.

A transformed surface occludes nothing: `subtract_opaque`, the desktop
fill of `compose_rect` and `scene_damage_surface` ignore its opaque
region. A commit of such a surface damages its whole transformed extent.
The scene frees the image of a surface when the surface returns to the
identity or is hidden, and when the surface is destroyed.

### Benchmark

The debug interface of version 4 has the request `animate(id, alpha,
scale, duration_ms)`. X12 animates the surface from its current values
with the standard curve. `compbench fade` and `compbench scale` use it on
their own window (`graphics-performance.md`), and `comp_bench` and
`comp_bench_hidpi` run both scenarios with `anim_percent` 100.

## Window animations (AN3)

`anim.c` has the effects, and the window manager and the shell call
them:

| Event | Effect | Duration | Curve |
|---|---|---|---|
| a toplevel maps (`wm_toplevel_committed`) | alpha 0 to 255, scale 0.96 to 1 | 150 ms | ease-out cubic |
| a toplevel closes | a snapshot: alpha to 0, scale to 0.96 | 120 ms | accelerate |
| a toplevel is minimized (`toplevel_set_minimized`) | scale and move into the button of the panel, alpha to 0 | 200 ms | standard |
| a toplevel is restored | from the button back to the identity | 200 ms | standard |

### Snapshots

A closed window no longer has a buffer. Its fade out therefore uses a
snapshot.
`anim_surface_closing` runs before the surface disappears: when the
role ends (`toplevel_gone`, `popup_gone`, `layer_gone`), when X12
dismisses a popup (`popup_dismiss_all`), when the client attaches a null
buffer, when the surface ends, and when a buffer of a disconnecting
client ends. libwire destroys the resources of a client newest first,
The buffers of a client that exits therefore end before its
surfaces.
`wire_client_closing` tells such a buffer from one that the client
destroys itself. The function renders the extent of the
window with the identity into an image (`scene_snapshot`, the same
rendering as the window capture). The snapshot is a `struct csurface`
with the role `ROLE_SNAPSHOT`, the extent of the window, its visual
parameters and its position in the stacking order (`scene_sort_key`).
Snapshots form a list of their own in `anim.c`. The scene adds the list
to its order (`scene_order`), and the other modules never see a
snapshot. A snapshot starts from the values on the screen. A window that
closes during its map animation therefore does not jump. The last step of
the animation removes the snapshot and frees its image. A minimized
window and a window of a locked session disappear without a snapshot.

A client that destroys the attached buffer, for example during a
resize, does not close the window. That event starts no animation.

### Minimize and restore

The panel sends the rectangle of the button of each window through
`set_task_rect` of the toplevel handle (`shell.md`). X12 records the
surface of the panel and the rectangle in the toplevel. The target
values (`task_visual`) place the centre of the extent on the centre of
the button, with the larger scale at which the extent fits into the
button and the alpha 0. Without a button the target is a rectangle of
160x28 pixels at the bottom centre of the screen.

A minimized toplevel remains visible while its animation runs. The end
of the animation hides the window and returns its visual parameters to
the identity. A restore starts from the button, or from the values on
the screen while the minimize animation still runs.

### Input

Input acts on the final state at once. A snapshot and a minimized
toplevel take no input (`scene_surface_at`). The geometry of a mapped
window is final from its first frame. A click during the map animation
therefore reaches the window at its final place.

### Tests

The boot case `x12_anim` (`kernel/tests/test_anim_gui.c`) runs X12 with
notifyd, the panel and `anim_percent` 400. `comptest popup` maps a
server decorated window and shows a popup over it after 1.5 s for 1.5 s.
The test polls one pixel during each animation. It requires a frame
other than the two end states, and for every fade a blend of the two end
colours in every channel, then the final state. The order is the map of
the window, the fade in and the fade out of the popup (AN4), the
minimize by the button of the decorations, the restore by the window
button of the panel, the close by Alt+F4, and the fade in and the fade
out of a notification card of the panel (AN4). compstat then reports
frames with animations. `mouse_click_now` and `alt_key_now` of
`gui_helpers.h` return without the wait for an idle system. The test
therefore observes the animation that the input starts. The QMP script of the case takes eight screendumps 100 ms
apart from the start of the minimize, for review. The last screendumps
show the start of the restore.

## Popup fades (AN4)

| Event | Effect | Duration | Curve |
|---|---|---|---|
| a popup maps (menus, the lists of combo boxes, tooltips) | alpha 0 to 255 | 90 ms | ease-out cubic |
| a popup ends or X12 dismisses it | a snapshot: alpha to 0 | 90 ms | accelerate |
| a notification card maps | alpha 0 to 255 | 90 ms | ease-out cubic |
| a notification card ends | a snapshot: alpha to 0 | 90 ms | accelerate |

The menus of the panel (the launcher, the calendar, the mixer, the power
menu, the input method menu and the history of the notifications) are
popups of the layer surface of the panel. The popup fades cover them.
The plan expected a popup layer and a notification layer. The shell
protocol has no such layers: the panel and the notification cards both
use layer 2. X12 records the namespace of `get_layer_surface` in the
layer (`ns`), and the cards of the namespace `notification` fade
(`fades` in `anim.c`). The panel, the desktop, the lock surface and the
candidate window of the input method do not fade.

A popup fades without a scale. Its snapshot has the place of the popup
in the stacking order, above its parent. A popup takes input from its
first frame, because its geometry is final at once.
