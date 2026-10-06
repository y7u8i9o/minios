# Windowing system

M17 adds a window server in user space, a client library with a widget
toolkit, a terminal emulator and demonstration applications, on top of
new kernel facilities: a mouse driver, framebuffer access from user
space, named message queues and shared memory, `poll`, a terminal layer
with pseudo terminals, and `sleep_ms`/`uptime_ms`.

## Kernel

- Mouse (`arch/x86_64/ps2mouse.c`): the auxiliary 8042 port is enabled and
  the device set to streaming mode. Both keyboard and mouse bytes arrive
  through port 0x60; bit 5 of the status register tells them apart, and
  both interrupt handlers drain the port accordingly. Since M47 packets
  are reported to the input core and read from `/dev/input/eventN`
  (`input.md`); `/dev/mouse` no longer exists.
- Framebuffer (`drivers/fbdev.c`): `/dev/fb0` reports the mode with
  `FBIOGET_INFO`, maps the framebuffer with `mmap` (write combining,
  `VM_DEVICE` regions whose frames are not reference counted, shared
  across `fork`), and hands the display over with `FBIO_ACQUIRE`, which
  stops the text console from drawing while its cell buffer retains
  receiving output. `FBIO_RELEASE`, or closing the descriptor, redraws
  the text console. `video=WxH[xBPP][@SCALE]` on the command line selects the mode
  through Limine; the default is 1024x768. `@2` is excluded from the Limine
  mode and parsed by the kernel into `bootinfo.fb_scale`: the console
  draws glyphs twice as large and `struct fb_info.scale` tells the
  compositor to compose a half size desktop and double every pixel
  (high density displays, see `build.md`). Since M32 the framebuffer is
  `fb_screen` (`display.md`): the virtio-gpu driver replaces the Limine
  one at boot, `FBIO_FLUSH` pushes rectangles and `FBIO_SET_MODE` changes
  the mode at run time.
- Keyboard: since M47 the server reads key codes from the keyboard's
  `/dev/input` node and grabs it (`input.md`); the `KBD_SCANCODES` mode
  and `/dev/kbd` no longer exist.
- IPC (`ipc/mqueue.c`, `ipc/shm.c`): named message queues carry fixed
  size messages (up to 256 bytes, 64 deep) with blocking send and receive;
  named shared memory objects are page sets that several address spaces
  map with `MAP_SHARED`; the object contains one reference per frame and
  every mapping adds its own, so unmapping remains uniform and kswapd never
  evicts a frame mapped twice. `fork` shares `VM_SHARED` regions without
  copy on write. Objects live until unlinked and closed by everyone.
  `poll` checks readiness through `file_ops.poll`, registers with each
  descriptor's object-local `poll_source` and sleeps on a private wait queue.
  A producer wakes only pollers registered on the object whose readiness it
  changed.
- Terminals (`drivers/tty.c`, `drivers/pty.c`): the line discipline
  moved out of the keyboard driver into `struct tty`: canonical editing,
  echo, control C to the foreground group (deferred to `ttyd` for the
  console, whose input arrives in interrupt context, and posted directly
  for pseudo terminals), control D, raw mode, window size, `TIOCSPGRP`
  and `TIOCGPGRP` which `tcsetpgrp` and `tcgetpgrp` now use through the
  descriptor's `ioctl`. `/dev/ptmx` allocates one of eight pairs; the
  master reads what the slave writes and feeds what it writes through
  the discipline of the slave `/dev/pts<n>`; `TIOCGPTN` gives the index.
  Closing the master hangs the slave up (readers get end of file, the
  foreground group gets `SIGHUP`). libc provides `openpty`.

## Window server

`user/wsrv/wsrv.c` maps `/dev/fb0`, acquires the display, switches the
keyboard to raw scancodes, creates the request queue `wsrv.req` and
polls it together with the mouse and the keyboard.

- Clients (`lib/libgui/src/client.c`) create the queue `wsrv.c<pid>` for
  events and replies, send `WM_CONNECT`, and create windows with
  `WM_CREATE`; the server allocates the shared surface `wsrv.s<id>`
  (32 bit RGB, stride equal to the width) which both sides map. Clients
  draw into the surface and send `WM_DAMAGE` rectangles. Other requests:
  `WM_MOVE`, `WM_SET_TITLE`, `WM_DESTROY`, `WM_DISCONNECT`. Events:
  `WM_KEY` (scancode, down or up, modifiers, translated character),
  `WM_MOUSE` (contents coordinates, buttons, move/down/up), `WM_FOCUS`,
  `WM_CLOSE`. Replies to a request arrive in order; events received
  while waiting for a reply are queued by the library.
- Compositing: windows have a z order, a title bar with the title and a
  close box, and a one pixel border. Damage rectangles are collected
  (merged into one when more than 32 accumulate); for each rectangle the
  desktop, the windows from bottom to top and the cursor are drawn into
  a back buffer and the rectangle is copied to the framebuffer.
- Input: a click focuses and raises the window under the cursor; a
  press on the title bar starts a drag, on the close box sends
  `WM_CLOSE`; other mouse events go to the focused window in contents
  coordinates. Keys go to the focused window with modifiers and the
  translation from the same keymap the console uses. New windows are
  cascaded from the top left and receive focus.
- The event log on standard output (`wsrv: window 1 created ...`,
  `wsrv: focus 2`, `wsrv: key 0x1e to 2`) is what the tests assert on.
  `SIGTERM` stops the server, which releases the display and restores
  the keyboard mode. `startgui [client]` runs the server with the
  terminal (or the named client) and returns to the text console when
  the server exits.

## Client library and toolkit

`lib/libgui/` builds `libgui.a`: `gfx` (fill, rectangle, line, 8x16 text,
clipped blit, rectangle helpers, and since G3 of
`docs/plan/compositor-performance.md` the rectangle set and the row
operations of `gui/pixel.h`, `graphics-performance.md`), the client
protocol, and `widgets`:
a tree of boxes (vertical or horizontal layout with padding, spacing
and expanding children), labels, buttons, text fields with a cursor,
list boxes with selection and scrolling, and canvases drawn by a
callback. `ui_run` redraws on demand, routes mouse and key events to
widgets, maintains keyboard focus, calls a periodic tick, and stops on
`WM_CLOSE`; unhandled keys reach `ui->on_key`.

Applications: `term` (the terminal emulator, described in
`terminal.md`), `clock` (canvas redrawn every second from
`uptime_ms`), `files` (the file manager, described in `files.md`),
`view` (scrolling text viewer), `mandel` (the progressive Mandelbrot
plotter on the application framework, described in `userland.md`).

## Tests

- `mouse`: packets injected into the driver become events; raw scancode
  mode returns untranslated bytes.
- `fb0`: `/bin/fbtest` maps the framebuffer, acquires the display, draws
  a checkerboard the kernel verifies pixel by pixel while the program
  contains the display, shares the mapping with a child, and releases.
- `mq`: `/bin/mqtest` exchanges messages and shared memory between a
  parent and a child, checks `poll` with and without timeout, message
  boundaries, oversize rejection and unlink semantics.
- `pty`: `/bin/ptytest` runs a shell on a slave, checks echo and output,
  control C, window size, hangup on master close and raw mode.
- `gui`: the server and `/bin/guitest` with two windows; injected mouse
  movement, clicks, a drag and a key press are checked against the
  server and client logs and against pixels of the composed screen.
- `gui_term`: the terminal window; a command typed through the keyboard
  driver reaches the shell, which writes a file, text is rendered in
  the window, and a resize reaches the shell (see `terminal.md`).

## M19 enhancements

### Kernel (stage 1)

- Mouse wheel: `ps2mouse_init` runs the IntelliMouse sequence (sample
  rates 200, 100, 80, then a device id request). A device answering id 3
  switches to four byte packets whose fourth byte carries the signed
  wheel delta in its low nibble; other devices retain three byte packets.
  `struct mouse_event` gained `dz`, positive towards the user. Tests use
  `ps2mouse_has_wheel` and `ps2mouse_set_wheel` to inject packets of the
  right length.
- Framebuffer layout: `struct fb_info` reports the size and shift of the
  red, green and blue channels from the Limine response, and `bpp` may
  be 24 or 32. `drivers/fbdev.h` contains `fb_pack_pixel`, `fb_unpack_pixel`,
  `fb_write_pixel` and `fb_read_rgb`, used by the console, the device and
  the tests. The console retains a 32 bit fast path and writes 24 bit
  pixels byte by byte. `video=WxHxBPP` selects a 24 bit mode.
- `SIGWINCH` (28) is ignored by default and sent to the foreground group
  of a terminal whose size changes through `TIOCSWINSZ`.

### Window server (stage 2)

`user/wsrv/` is split into `wsrv.c` (clients, windows, requests, main
loop), `compose.c` (damage, visibility, decorations, framebuffer copy),
`input.c` (mouse and keyboard) and `taskbar.c` (task bar, launcher,
clipboard). Windows and clients are allocated individually; the window
table is a growable array of pointers.

- Surfaces are named `wsrv.s<id>.<generation>`. `WM_RESIZE` (or a drag of
  the grip in the bottom right corner, applied on release) frees the old
  surface, allocates the next generation filled with light grey, and
  sends `WM_RESIZED(width, height, generation)`; the client library maps
  the new surface before the application sees the event. Objects live
  until unmapped, so the client's old mapping remains valid meanwhile.
  Windows have minimum sizes (`WM_CREATE` c, d or `WM_SET_MINSIZE`,
  default 64x32).
- Title bar boxes from the right: close, maximize, minimize. Maximize
  fills the desktop above the task bar and remembers the previous frame;
  the box restores it. Minimized windows are hidden, retain their z order,
  and are listed in the task bar with a leading underscore. Window
  positions are clamped so that part of the title bar remains on the
  desktop.
- Shortcuts handled by the server: Alt+Tab raises and focuses the lowest
  visible window so repeated presses visit every window, Alt+F4 sends
  `WM_CLOSE` to the focused window, Alt+drag moves a window from
  anywhere inside it, Escape closes the launcher menu.
- Wheel packets become `WM_MOUSE` events of kind `WMOUSE_WHEEL` with the
  delta in `c`, delivered to the window under the cursor.
- Clipboard: the server creates the shared object `wsrv.clip` (64 KiB).
  `gui_clipboard_set` writes the text there and sends `WM_CLIP_SET(len)`;
  `gui_clipboard_get` sends `WM_CLIP_GET` and copies `WM_CLIP_DATA.a`
  bytes out. Text only, single user, no locking.
- Task bar (28 pixels at the bottom): a Menu button, one button per
  window (click focuses, minimizes the focused window, or restores a
  minimized one), and an uptime clock updated each second. The menu
  lists `/etc/launcher` (`title=path` lines); choosing an entry forks,
  closes the server's descriptors and executes the program. `SIGCHLD`
  interrupts `poll` so children are reaped in the main loop.
- Compositor: per damage rectangle, the desktop is painted only where no
  window frame lies; then windows are drawn from the bottom up, each
  clipped to the pieces of its frame not covered by the windows above
  (rectangle subtraction, at most 64 pieces, falling back to overdraw
  beyond that). A window whose whole frame is covered is skipped and
  logged once as `wsrv: window N culled`. Decorations draw through a
  view of the back buffer restricted to the clip so gfx clips them.
  The back buffer is 0x00RRGGBB; `screen_copy` converts to the reported
  channel layout and to 24 bit when the framebuffer is not native.
- Log lines added for the tests: `resized WxH`, `maximized`, `minimized`,
  `restored`, `culled`, `alt-tab`, `wheel D in N`, `menu opened/closed`,
  `launch PATH`, `taskbar click window N`, `clipboard set N bytes`.

### Tests

- `mouse_wheel`: four byte packets carry `dz`; a three byte device still
  works.
- `fb_format`: the `fb0` case in a 24 bit mode, the program packing
  pixels through the reported layout.
- `pty`: a window size change on the master delivers `SIGWINCH` to the
  slave's foreground group. The case also waits for the prompt before
  hanging up, since an orphan of the shell would otherwise leak while no
  init runs.
- `gui_resize`: grip drag, maximize, restore, close, with pixel checks
  of the green refill the client performs after each `WM_RESIZED`.
- `gui_wm`: wheel delivery, Alt+Tab, minimize and restore from the task
  bar, culling when a maximized window covers another, the launcher
  starting the clock, Alt+F4 closing every window.
- `gui_clip`: two clients exchange text through the clipboard.

### Client library and toolkit (stages 3 and 5)

- Bitmap fonts: `struct font` describes 256 glyphs with per glyph
  advance and width and 32 bit wide rows. `gfx_font_builtin` wraps the
  8x16 font; `gfx_font_load` reads `.mfnt` files produced by
  `tools/genfont/genfont.py mfnt <ttf> <px> <out>` from the DejaVu faces
  in `third_party/dejavu/` (`/usr/share/fonts/sans18.mfnt`, `mono20.mfnt`).
  `gfx_text_font`, `gfx_text_width_font` and `gfx_text_index_font` draw
  and measure with any font; the older 8x16 calls remain.
- Buffers (G8 of `docs/plan/compositor-performance.md`): a window draws
  straight into its shared memory pool. There is no private copy. The
  pool has three slots, each one window buffer plus a quarter for growth.
  A memfd page gets memory at its first use, so a window normally uses
  two slots. After a commit the compositor may read the committed slot.
  `gui_begin_paint` therefore moves the next frame to a free slot. It
  copies the regions that changed since that slot was last current, and
  it restores the raw pixels of the rounded frame corners. If both slots
  remain busy for 100 ms, the third slot is used. A commit blends the
  frame corners over the chrome and sends one damage rectangle per
  changed region. `gui_window.surf` points at the contents in the
  current slot. A resize to a size beyond the slot capacity creates a
  new pool. The old pool is destroyed after the compositor releases its
  buffers. The slot logic is in `lib/libgui/src/buffers.c` without
  protocol calls, and `make check-libgui` tests it against a reference
  picture.
- Toolkit (`widgets.c`, `widgets_text.c`, `widgets_menu.c`): `ui->font`
  selects the font (`ui_set_font`), line heights follow it. New widgets:
  scroll bar (`ui_scrollbar`, `ui_scrollbar_set`, thumb dragging, arrows
  and page keys, wheel), check box, text area (`ui_textarea`, growable
  buffer, multi line cursor movement, selection with Shift and the
  mouse, `readonly`), menu bar (`ui_menubar`, `ui_menu`, `ui_menu_item`,
  drop downs drawn last, keyboard navigation, Escape). Text fields and
  areas share the editing code: Ctrl+A, Ctrl+C, Ctrl+X, Ctrl+V through
  the server clipboard; Enter in a field calls `on_select` when set,
  otherwise `on_change`. List boxes and text areas draw a scroll bar
  when their contents overflow and react to the wheel. `ui_dialog` and
  `ui_prompt` open modal windows with their own event loop; `ui_run`
  ignores events of other windows. `WM_RESIZED` relayouts the tree.

### Applications (stage 4)

- `term`: the cell grid follows the window size (cells from the mono20
  font when present, else 8x16), `WM_RESIZED` reallocates the grid,
  retains the overlapping contents and sets the pseudo terminal size with
  `TIOCSWINSZ`, which raises `SIGWINCH` in the shell's group. Lines
  scrolled off the top enter a 1000 line ring; the wheel and
  Shift+PageUp/PageDown move the view (`term: view N` in the log), and
  new output or a key returns to the live screen. Minimum size 20x5.
- `view`: a read only text area with a scroll bar, a File menu (Open
  through `ui_prompt`, Quit) and a View menu switching between the
  built in and the sans18 font. `files` gains the list box scroll bar.
- `stty size` prints the terminal size (used by the tests).

## M26: the client library on the compositor

`wsrv`, `gui/proto.h`, the message queue transport and the shared
clipboard object are gone. `lib/libgui/src/client.c` implements the same
`gui_*` API over libwire:

- `gui_connect` connects to the `display` socket (non blocking), binds
  the globals, creates a pointer, a keyboard and a data device.
- A `gui_window` is a surface with the toplevel role and a pool of two
  buffers in a memfd. Since B2 of `docs/plan/desktop-panel.md` every
  window and dialog carries the app_id `getprogname()`, the last part of
  `argv[0]`, and the panel shows the icon of that program. The `gui`
  module of Lua sets the program name to the script of `arg[0]` when the
  script creates its application, so a Lua window carries the name of
  its script, not the name of the interpreter. The application draws into `gui_window.surf` (a
  private buffer) and marks rectangles with `gui_damage`; `gui_flush`
  (called by `gui_next_event` and by the framework after painting)
  copies the union of the damage since the buffer was last shown into a
  free buffer, attaches it, requests a frame callback and commits. A
  new commit waits for the previous frame's `done` and for a released
  buffer, so a client renders at most once per compositor frame.
- `configure` events with a size resize the private buffer and the
  pool (the old pool is destroyed only after the next commit, so the
  compositor never sees the surface without a buffer) and queue
  `WM_RESIZED`; `close` queues `WM_CLOSE`; keyboard `enter` and `leave`
  queue `WM_FOCUS`; pointer events queue `WM_MOUSE` with contents
  coordinates and a button mask; `key` events queue `WM_KEY` with the
  raw code, the modifiers and the character from the seat's keymap.
- The clipboard uses the data device: `gui_clipboard_set` creates a
  source offering `text/plain`, sets the selection and waits until the
  compositor fetched a copy; `gui_clipboard_get` reads the current
  selection offer through a pipe (or answers from the process's own
  text when it owns the selection).
- Drag and drop uses the same data device (`gui_drag_start`,
  `WM_DRAG_ENTER`, `WM_DRAG_MOTION`, `WM_DRAG_LEAVE`, `WM_DROP`,
  `WM_DRAG_END`, `gui_drag_accept`, `gui_drag_peek`, `gui_drop_data`),
  described in `dnd.md`.
- The compositor retains a copy of every selection (`data.c`, fetched
  when it is set) and serves it itself after the owner exits, so copy,
  quit, paste works. A client that dies is detected through `POLLHUP`
  on its socket and destroyed with its surfaces.

`startgui` starts the compositor, the panel and the program. The GUI
boot tests run on X12 and the panel; their expectations use the `x12:`
and `panel:` log lines.

## Client side decorations (`lib/libgui/src/csd.c`)

Toplevels of libgui draw their own chrome, the way GTK 4 does under
Wayland, in a light style that matches the rest of the toolkit. The
drawing surface of a window is the whole buffer: a `CSD_MARGIN` (16 px)
band for the shadow around the frame, and inside the frame a
`CSD_HEADER` (36 px) header bar above the contents; `gui_window.surf` is a view of the contents, so applications,
the framework and `gui_damage` retain their contents coordinates, and the
client layer adds the offset to damage, popup anchors, regions and
pointer coordinates. The buffers are ARGB; `csd_copy` makes the frame
opaque while copying into them and blends the contents over the chrome
at the four rounded corners (`CSD_RADIUS` 6). The margins contain the
outline (one logical pixel of 20 percent black) and the shadow (black,
`(1 - t)^2` over an 8 px reach, shifted 2 px down, half as strong for
inactive windows); they are painted once per resize or state change,
not per frame. Since G7 of `docs/plan/compositor-performance.md` the
outline and the shadow come from two profiles of 16 bit alpha values by
the squared distance of a pixel centre to the frame, in half pixels, and
the rounded corners and the button discs from the coverage tables of
`gui/pixel.h`; the corner blend of `csd_copy` uses integer arithmetic.
Before, every chrome pixel computed two square roots and float blends.
`make check-libgui` compares the result with the former float code
(`tests/test_csd.c`): every channel lies within one of it at the scales
1 to 3.

The header bar is flat (`0xebebeb`, `0xfafafa` in the backdrop), with a
hairline under it, the title centred in DejaVu Sans 13 px, and three
20 px round buttons at the right: close, maximize (a restore glyph when maximized) and minimize,
with a hover shade. The toolkit handles the pointer over the chrome:
the header bar starts `toplevel.move` (a double click toggles
maximized), an 8 px zone outside the frame starts `toplevel.resize`
with the edges (corners within 24 px take two), the buttons act on
release. A square at each corner reaches 12 px outside the frame and
6 px inside, over the rounded corner, and takes two edges as well
(`gui_resize_edges` in `gfx.c`, which the compositor shares). The
compositor learns the frame through `set_window_geometry`, an opaque
region of the frame minus its corner squares, and an input region of the
frame with the resize zone and the four corner squares
(`gui_resize_region`). Maximized windows drop
the margins and the corners; the configure states drive `active` and
`maximized`. A compositor answering `decoration.mode` with server side
decorations turns all of this off and the window is plain again.


The default theme uses the same light neutral greys as the chrome:
the window body is the header bar's `0xebebeb`, borders `0xb0b0b0`,
buttons `0xdcdcdc` without borders (hover `0xd0d0d0`, pressed
`0xbcbcbc`), accent `0x3c78c8`; tabs are marked by an accent underline,
scrollbar tracks and progress bars have no frame, and the theme radius
is 5 px. A top level window retains the theme padding around its
contents.
