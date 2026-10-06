# X12 display server

X12 is the display server and window manager. Its implementation remains
in `user/compositor/` for source-history continuity, while the installed
server is `/bin/x12` and its log prefix is `x12:`. It replaces the M17
window server's core (`wsrv` remains as historical code until M26).
The server owns the framebuffer, input devices, surface lifecycle,
window roles, and composition.

## Log

`comp_log` writes `x12: ...` lines to `/var/log/x12.log`, truncated
when the server starts (the directories are created if missing; when
the file cannot be opened, on a read-only root, the lines go to
standard output instead). The option `-s` mirrors the log to standard
output, which reaches the serial line; the boot tests start the server
with it and match the lines. Frequent lines (each frame, the ten second
frame count, each key delivered, pointer enter, buttons, axes, each
commit and buffer release) go through `comp_debug`, which writes only
while the `verbose` setting is on: the option `-v` or the debug
interface. The protocol cases (`comp_*`) start the server with `-s -v`.
`startgui` starts it without options, so a desktop session leaves the
serial line to the kernel and the programs.

## Structure

- `main.c`: the poll loop over the listening socket, the client
  sockets, the `/dev/input` devices (`input.c`, `input.md`) and a `timerfd`
  firing every 16 ms; the log (below); `SIGTERM` stops it and prints
  frame statistics.
- `surface.c`: the globals `compositor`, `shm` and `output`; pools map
  the passed memfd with `mmap` and are shared by their buffers
  (`XRGB8888` = 1, `ARGB8888` = 2, stride at least 4 times the width,
  geometry checked against the pool size); surfaces carry a pending
  state (buffer, damage rectangles, frame callbacks) that `commit`
  moves into the current state. A commit with a new buffer releases
  the previous one (`buffer.release`) and damages the old and new
  extents, or, when the geometry is unchanged (the usual frame of a
  double buffered client, whose decorations and shadow remain as drawn),
  the damage rectangles of the commit; a buffer without damage
  rectangles damages the whole surface. A commit without a new buffer
  damages the listed rectangles. A new scale, transform or opaque region
  damages the whole surface. A frame callback adds no damage (G5 of
  `docs/plan/compositor-performance.md`; before, every buffer redrew the
  whole surface and every frame callback damaged it). A damage rectangle
  lying entirely inside one opaque surface skips the desktop fill. Surfaces are placed in a cascade until M25 gives them
  roles and positions; a surface is mapped once it has a buffer.
- `scene.c`: the damage as a `rect_set` of up to 32 disjoint rectangles
  (`gui/gfx.h`), occlusion culling of surfaces below opaque surfaces,
  per rectangle composition (desktop fill, surfaces bottom up, the
  cursor), and one present of all rectangles of a frame through
  `backend_present`. The opaque region of an `ARGB8888` surface decides
  its opacity: every opaque span of a row is copied with the alpha
  cleared, and the rest is blended with `pixel_over`. The built-in arrow
  is an ARGB image per scale, blended with `pixel_over`.
- `data.c`: the data device, with the selection and its stored copy
  (`gui.md`) and drag and drop: the drag icon, the offers to the surfaces
  under the cursor, the copy and move actions chosen from both sides and
  the modifiers, the drop, and the cancel by Escape (`dnd.md`).
- `trace.c`: the `tracer` global of `protocol/debug.xml`. `start`
  installs libwire's trace hook while at least one tracer runs, sends a
  `client` event for every other connected client, and turns every
  request and event of the other clients into a `message` event with a
  sequence number, the uptime, the client number, the direction, the
  object and the arguments formatted by `wire_format_args`. `client`
  events follow when a client connects, reports its pid through the
  shell or disconnects. The traffic of a client that traces is never
  traced. Two tracers therefore cannot trace the messages of each other
  without end. A flag prevents the trace function from tracing the events
  that it queues itself. When more than 48 KiB
  are queued to a tracer, its messages are counted instead of queued
  and reported by one `dropped` event once it reads again, so a slow
  tracer never makes libwire drop a message of its own. Up to eight
  tracers may be bound.
- `debug.c`: the `debug`, `settings` and `screencopy` globals of
  `protocol/debug.xml`. `screencopy.capture` copies the back buffer into a
  client buffer of the screen size, with or without the pointer,
  `get_windows` lists the toplevels and `capture_window` draws one of
  them alone with its shadow (`scene_copy_screen`,
  `scene_render_window`, `images.md`). `seat.c` starts `/bin/screenshot`
  on Print Screen, Shift+Print Screen and Super+Shift+3, 4 and 5, and
  passes Print Screen to an overlay layer surface that has the keyboard
  focus.
- `backend_fb.c`: the framebuffer mapping, the 32 bit back buffer and
  the conversion copy for non native pixel layouts (from the window
  server). When `/dev/fb0` reports a scale (`video=WxH@2`), the back
  buffer and every coordinate above it are logical pixels, the screen is
  `width/2` by `height/2`, and the flush expands each logical row once
  (M32). Since M33 the back buffer contains device pixels: the scene
  composes at `screen_scale` device pixels per logical pixel, buffers
  with the output's scale are copied 1:1 and others resampled, and
  decorations and the cursor are drawn at the scale (`display.md`).
  Every copied rectangle is flushed with `FBIO_FLUSH` (a no-op on plain
  VGA), and `backend_set_mode` changes the resolution through
  `FBIO_SET_MODE`.

A buffer committed while a configure is unacknowledged is accepted when
it has the surface's current geometry (a frame sent before the configure
arrived, see `display.md`); a buffer of another size is a protocol
error. A layer surface records the last four configures it was sent. Its
client may acknowledge an older one that arrived before a newer one, as
the desktop does when the panel maps and reconfigures it during its first
configure, and may then commit a buffer of that older size. The newer
configure remains pending until the client acknowledges it. libwire's server logs every protocol error it posts and every
failed send on stderr.

Layer surfaces on the overlay layer (3) are stacked above the top
layers and the popups of the panel, below the input method candidates
and drag icons. When an overlay layer surface with keyboard
interactivity is mapped, X12 gives it the keyboard focus, and when the
surface is destroyed, X12 gives the focus back to the active toplevel.
The screenshot interface depends on both rules. `set_margin` positions a
layer surface relative to the desktop area, which is the screen minus
the exclusive zones. X12 places each anchored edge at the given margin
from the corresponding edge of that area. A surface anchored to the
bottom is therefore placed above the panel. A layer surface without
margins is positioned as before, against the screen edge when it is
anchored on one side and at the desktop area when it is anchored on two
opposite sides. A layer surface without an anchor on an axis is centred on the
screen on that axis, as in the layer shell of wlroots. A size of 0 takes
the dimension of the desktop area. A layer surface of width 0 with the
anchors left and right therefore receives the new width after a mode
change. A layer surface with a nonzero width receives that width again.

A layer surface records the anchor and the exclusive zone of its last
placement. A commit of a mapped layer that changed either, such as the
panel that moves to the top edge (B6 of `docs/plan/desktop-panel.md`),
calls `shell_output_changed`: every layer is placed and configured again
and maximized windows take the new desktop area, as after a mode change.
The log line is `layer surface N moved to X,Y WxH`.

X12 polls `/dev/fb0` for the size requests of the host display. With the
setting `display_follow` X12 changes the mode to the requested size
(`display.md`, V3 of the 0.6.0 release).

While an overlay layer surface has the keyboard focus, Alt+Tab and Alt+F4
go to the surface instead of cycling or closing the toplevels below it,
so that the login window and the authentication dialog (`users.md`)
retain the keyboard. A click on the title bar of a server decorated
toplevel reaches the title bar only when no surface above it covers the
point: a toplevel higher in the stack, a layer surface of the top or
overlay layer, or a popup (`covers_decorations` in `seat.c`). Before
2026-10-05 X12 compared only the stack positions, which layer surfaces do
not have, and a title bar below the panel or an overlay took the click.

A window with a text input context receives its key repeats from X12
instead of libgui (`input.md`).

## Frame clock and callbacks

Since G6 of `docs/plan/compositor-performance.md` the frame clock runs
only while work is pending. After each pass of the main loop,
`schedule_frame` composes at once when damage or a frame callback is
pending and `frame_ms` (16 ms by default) have passed since the start of
the last frame. Otherwise it arms the timerfd once for the rest of the
interval, and it disarms the timer when nothing is pending. A frame
composes and flushes the damage, and then every frame callback
registered by a commit since the previous frame receives `callback.done`
with the time in milliseconds. A callback whose commit added no damage
receives `done` in a frame without a composition (G5). `frame_ms` is the
minimum interval between frames.

The poll timeout is the earliest deadline of the modules with timed
work: the next key repeat of the text input (`seat_next_deadline`), the
next ping, hang and end of a snooze (`hang_next_deadline`) and the
timeout of a key sent to the input method (`im_next_deadline`). Without
such a deadline poll waits for input and clients alone. Before G6 a
periodic 16 ms timer woke X12 also when it was idle, about 64 times a
second, and a commit waited up to 16 ms for the next tick. `comp_idle`
requires five idle seconds with one window to compose nothing and to
wake X12 fewer than 40 times. Clients are flushed with
the presentation event. The framebuffer ABI currently exposes a mapped
front buffer and has no page-flip or vblank primitive, so this is a
software presentation clock rather than a claim of hardware vsync.

## P0 and P1 semantics

P0 makes the wire state explicit: a role sends an initial configure and
the client must acknowledge it before committing a new-size buffer;
pending surface state is atomic at commit; buffer scale, quarter-turn
transforms, attach offsets and buffer-space damage are converted before
composition; input and opaque regions participate in hit testing and
occlusion; cursor surfaces are accepted only from the focused client and
serial; and input, popup, move/resize, selection and drag serials are
validated against their owning client.

P1 adds compositor-managed popup and modal roles, including constrained
flip/slide/resize placement and popup grabs; output scale/transform/done
events and client output tracking; UTF-8 editing and outline-font cmap,
fallback and combining-mark support; the text-input protocol with a
built-in Ctrl+Shift+U Unicode preedit; and server-side shadows. The relay
to the input method daemon (`inputmethod.c`) is described in `ime.md`.

## Tests

`comp_core`: `/bin/comptest` connects, binds the globals, reports the
shm formats and the output mode, creates a two buffer pool from a
memfd, commits red, waits for `done`, commits green and receives the
release of the red buffer, commits partial damage (one blue pixel),
and destroys its objects; the kernel checks the pixels on the
framebuffer and the compositor's log.

## Robustness (after M26)

- X12, the panel and libgui clients ignore `SIGPIPE`; a
  write to a vanished peer is reported as an error and the client (or
  the connection) is dropped instead of the process dying.
- Client sockets are accepted non blocking, so one client cannot stall
  the display server; when a client's socket is full, libwire drops the
  event instead of blocking or overflowing its buffer. The terminal leaves
  its pseudo terminal master non blocking for the same reason.
- Unresponsive clients (`user/compositor/hang.c`): X12 sends `shell.ping`
  to every client once a second and the client answers with
  `shell.pong` (libgui does this in its event dispatch). A client that
  has not answered for three seconds, or whose socket has remained full
  that long, is shown as not responding: its toplevels are dimmed and
  carry a dialog drawn by the server with "<title> is not responding",
  Wait and Force quit. Wait hides the dialog for fifteen seconds; Force
  quit sends `SIGKILL` to the pid the client reported with
  `shell.set_pid` (libgui sends it at connect) and drops the connection;
  a client that answers again gets its windows back unchanged. Clicks on
  a dimmed window go nowhere. Test: `comp_hang` (a `comptest` client
  that hangs for six seconds, Wait, recovery, then one that hangs for
  good and is killed through the button).
- Closing the `/dev/input` descriptors drops their grabs (M47; before, closing `/dev/kbd` left raw scancode mode, `kbddev_release` in
  `kernel/fs/devfs.c`), so a compositor that dies leaves the console
  keyboard usable; `startgui` ends the session when any of X12, the
  panel or the program exits.
- Logging: one `x12: N frames in the last 10 s` line instead of a
  line per frame; `slow frame: N us` lines remain for frames over 20
  ms; `frame stats` at exit. Tests: `gui_dead_client`,
  `gui_kbd_restore`.
- Frame statistics (`stats.c`, G1 of `docs/plan/compositor-performance.md`):
  counters, times in microseconds and latencies of the frames since the
  last reset, sent by the `debug` interface of version 2 and printed by
  `compstat` (`graphics-performance.md`). Tests: `comp_bench`,
  `comp_bench_hidpi`.

## Panel popups and session survival

A popup that X12 dismisses (an outside click sends
`popup.done`) still exists as a client object. The panel destroys the
popup and its surface in the `done` handler, because a later
`shell.get_popup` on the same surface would be refused with
"surface already has a role", and a protocol error disconnects the
client. `startgui` treats a normal panel exit as "Log out" and restarts
a panel that ends with an abnormal status (up to three times), so a
panel defect no longer ends the session. The `comp_panel` boot test
covers the dismiss and reopen sequence. Popup creation is asynchronous:
the panel records the button-down serial, acknowledges the configure,
and requests the grab only after the server has supplied that configure.
