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
  extents, or only the contents when the geometry is unchanged (the
  usual frame of a double buffered client, whose decorations and shadow
  stay as drawn); a commit without a new buffer damages the listed
  rectangles. A damage rectangle lying entirely inside one opaque
  surface skips the desktop fill. Surfaces are placed in a cascade until M25 gives them
  roles and positions; a surface is mapped once it has a buffer.
- `scene.c`: the damage list with merging of touching rectangles,
  occlusion culling of surfaces below opaque (`XRGB8888`) surfaces,
  per rectangle composition (desktop fill, surfaces bottom up with
  alpha blending for `ARGB8888`, the cursor), and frame statistics.
- `trace.c`: the `tracer` global of `protocol/debug.xml`. `start`
  installs libwire's trace hook while at least one tracer runs, sends a
  `client` event for every other connected client, and turns every
  request and event of the other clients into a `message` event with a
  sequence number, the uptime, the client number, the direction, the
  object and the arguments formatted by `wire_format_args`. `client`
  events follow when a client connects, reports its pid through the
  shell or disconnects. The traffic of a client that traces is never
  traced, so two tracers cannot feed each other, and a flag keeps the
  hook from tracing the events it queues itself. When more than 48 KiB
  are queued to a tracer, its messages are counted instead of queued
  and reported by one `dropped` event once it reads again, so a slow
  tracer never makes libwire drop a message of its own. Up to eight
  tracers may be bound.
- `debug.c`: the `debug`, `settings` and `screencopy` globals of
  `protocol/debug.xml`. `screencopy.capture` copies the back buffer into a
  client buffer of the screen size (`images.md`). `seat.c` starts
  `/bin/screenshot` when Print Screen is pressed without Alt.
- `backend_fb.c`: the framebuffer mapping, the 32 bit back buffer and
  the conversion copy for non native pixel layouts (from the window
  server). When `/dev/fb0` reports a scale (`video=WxH@2`), the back
  buffer and every coordinate above it are logical pixels, the screen is
  `width/2` by `height/2`, and the flush expands each logical row once
  (M32). Since M33 the back buffer holds device pixels: the scene
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
configure stays pending until the client acknowledges it. libwire's server logs every protocol error it posts and every
failed send on stderr.

## Frame clock and callbacks

The frame timer expires every 16 ms. When damage exists the scene is
composed and flushed; only after that successful flush does every frame
callback registered by a commit since the previous frame receive
`callback.done` with the time in milliseconds. Clients are flushed with
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
built-in Ctrl+Shift+U Unicode preedit; and server-side shadows. The input
methods for Japanese and Chinese (`ime.c`) are described in `ime.md`.

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
  event instead of blocking or overflowing its buffer. The terminal keeps
  its pseudo terminal master non blocking for the same reason.
- Unresponsive clients (`user/compositor/hang.c`): X12 sends `shell.ping`
  to every client once a second and the client answers with
  `shell.pong` (libgui does this in its event dispatch). A client that
  has not answered for three seconds, or whose socket has stayed full
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
  line per frame; `slow frame` lines remain for compositions over 20
  ms; `frame stats` at exit. Tests: `gui_dead_client`,
  `gui_kbd_restore`.

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
