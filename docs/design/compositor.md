# Compositor

`user/compositor/` replaces the M17 window server's core (`wsrv`
remains until M26). M24 implements the core: clients, shared memory
pools and buffers, surfaces with double buffered state, a frame clock,
and composition to the framebuffer.

## Structure

- `main.c`: the poll loop over the listening socket, the client
  sockets, `/dev/mouse`, `/dev/kbd` (raw scancodes) and a `timerfd`
  firing every 16 ms; `comp: ...` log lines for the tests; `SIGTERM`
  stops it and prints frame statistics.
- `surface.c`: the globals `compositor`, `shm` and `output`; pools map
  the passed memfd with `mmap` and are shared by their buffers
  (`XRGB8888` = 1, `ARGB8888` = 2, stride at least 4 times the width,
  geometry checked against the pool size); surfaces carry a pending
  state (buffer, damage rectangles, frame callbacks) that `commit`
  moves into the current state. A commit with a new buffer releases
  the previous one (`buffer.release`) and damages the old and new
  extents; a commit without a new buffer damages the listed
  rectangles. Surfaces are placed in a cascade until M25 gives them
  roles and positions; a surface is mapped once it has a buffer.
- `scene.c`: the damage list with merging of touching rectangles,
  occlusion culling of surfaces below opaque (`XRGB8888`) surfaces,
  per rectangle composition (desktop fill, surfaces bottom up with
  alpha blending for `ARGB8888`, the cursor), and frame statistics.
- `backend_fb.c`: the framebuffer mapping, the 32 bit back buffer and
  the conversion copy for non native pixel layouts (from the window
  server).

## Frame clock and callbacks

The frame timer expires every 16 ms. When damage exists the scene is
composed and flushed; then every frame callback registered by a
commit since the previous frame receives `callback.done` with the
time in milliseconds, and the clients are flushed. Clients that draw
on frame callbacks therefore render at most once per compositor
frame, which bounds the work during bursts of damage.

## Tests

`comp_core`: `/bin/comptest` connects, binds the globals, reports the
shm formats and the output mode, creates a two buffer pool from a
memfd, commits red, waits for `done`, commits green and receives the
release of the red buffer, commits partial damage (one blue pixel),
and destroys its objects; the kernel checks the pixels on the
framebuffer and the compositor's log.

## Robustness (after M26)

- The compositor, the panel and libgui clients ignore `SIGPIPE`; a
  write to a vanished peer is reported as an error and the client (or
  the connection) is dropped instead of the process dying.
- Client sockets are accepted non blocking; a client whose socket
  stays full for two seconds (it stopped reading, for example because
  it is blocked elsewhere) is dropped, so one client cannot stall the
  compositor. The terminal keeps its pseudo terminal master non
  blocking for the same reason.
- Closing `/dev/kbd` leaves raw scancode mode (`kbddev_release` in
  `kernel/fs/devfs.c`), so a compositor that dies leaves the console
  keyboard usable; `startgui` ends the session when any of the
  compositor, the panel or the program exits.
- Logging: one `comp: N frames in the last 10 s` line instead of a
  line per frame; `slow frame` lines remain for compositions over 20
  ms; `frame stats` at exit. Tests: `gui_dead_client`,
  `gui_kbd_restore`.

## Panel popups and session survival

A popup that the compositor dismisses (an outside click sends
`popup.done`) still exists as a client object. The panel destroys the
popup and its surface in the `done` handler, because a later
`shell.get_popup` on the same surface would be refused with
"surface already has a role", and a protocol error disconnects the
client. `startgui` treats a normal panel exit as "Log out" and restarts
a panel that ends with an abnormal status (up to three times), so a
panel defect no longer ends the session. The `comp_panel` boot test
covers the dismiss and reopen sequence.
