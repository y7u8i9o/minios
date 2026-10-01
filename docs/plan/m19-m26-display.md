# Milestones M19 to M26: windowing, fonts, toolkit and display server

## M19. Windowing system enhancements (completed 2026-08-30)

Decisions (2026-08-30): the GUI stays on demand through `startgui`; the
protocol keeps 256 byte messages while the window and client tables
become dynamic; verification continues with the serial event log plus
CRC32 and pixel checks of framebuffer regions; the work forms one
milestone with one design document (`docs/design/gui.md` extended) and
one boot test per stage.

Stage 1, kernel input and output (done 2026-08-30):
- Mouse scroll wheel: the IntelliMouse enable sequence (sample rate 200,
  100, 80, then device id 3), four byte packets, a `dz` field in
  `struct mouse_event`. Devices that stay at id 0 keep three byte packets.
- Framebuffer format: `FBIOGET_INFO` gains the red, green and blue mask
  sizes and shifts taken from the Limine framebuffer response. `fbcon`
  and `wsrv` compose pixels through the reported layout, so 24 bit and
  BGR modes work. 32 bit RGB remains the fast path.
- Window size changes on pseudo terminals already exist through
  `TIOCSWINSZ`; the slave gets `SIGWINCH` to its foreground group.

Stage 2, protocol and server (`user/wsrv/`, done 2026-08-30):
- `WM_RESIZE(id, w, h)` from clients and `WM_RESIZED(w, h, generation)`
  to clients: the server allocates a new surface
  `wsrv.s<id>.<generation>` and unlinks the old one at once (it lives
  until the client unmaps it); the client library maps the new one
  before the application sees the event. Windows carry minimum sizes.
- Decorations: a resize handle in the bottom right corner, maximize and
  minimize boxes in the title bar. Maximize fills the desktop area above
  the task bar and remembers the previous frame. Minimized windows are
  hidden and listed in the task bar. Windows are clamped so that their
  title bar stays on screen.
- Server keyboard shortcuts: Alt+Tab cycles focus in z order, Alt+F4
  sends `WM_CLOSE`, Alt+drag anywhere in a window moves it.
- Clipboard: contents always go through the shared memory object
  `wsrv.clip` (64 KiB); `WM_CLIP_SET(len)` and `WM_CLIP_GET` returning
  `WM_CLIP_DATA(len)`. Only text is supported.
- Task bar drawn by the server at the bottom of the screen: one button
  per window showing the title (click focuses or restores), a launcher
  menu listing programs from `/etc/launcher` (one `title=path` line per
  entry), and the clock.
- Compositor: occlusion culling skips windows fully covered by opaque
  windows above them within a damage rectangle, and clips each window's
  drawing to its visible region computed by subtracting the frames of
  windows above it. Dynamic window and client arrays replace the fixed
  tables.
- Wheel events reach the window under the cursor as `WM_MOUSE` with
  kind `WMOUSE_WHEEL` and the delta in `d`.

Stage 3, client library and toolkit (`libgui/`, done 2026-08-30):
- Fonts: `tools/genfont/genfont.py` gains a mode producing a variable
  width font file (`.mfnt`: header, glyph advances, bitmaps) from a
  larger source face, built into `user/etc/fonts/`. `gfx` loads fonts
  from the filesystem (`gfx_font_load`) and draws with either font; the
  built in 8x16 font stays the default and the fallback.
- Widgets: scroll bar, check box, text area with multi line editing,
  menu bar with drop down menus, modal message dialog. Text fields and
  areas support selection, and Ctrl+C, Ctrl+V and Ctrl+X through the
  clipboard. Widgets react to wheel events (list box, text area, scroll
  bar).
- Layout on resize: `ui_run` handles `WM_RESIZED` by remapping the
  surface and relayouting the tree. Boxes support minimum sizes.

Stage 4, applications (done 2026-08-30):
- Terminal: cell grid derived from the window size, resize propagates
  through `TIOCSWINSZ`, a 1000 line scrollback scrolled with the wheel
  and Shift+PageUp/PageDown, the larger font when available.
- `files` and `view` use the scroll bar and the resize path; `view`
  gains a menu bar with open and font size entries. A `launcher` entry
  file lists `term`, `files`, `view`, `paint`, `pong`, `clock`.

Stage 5, double buffering (done 2026-08-30): applications draw into a
private buffer; `gui_damage` copies the rectangle into the shared
surface, so the compositor never shows a half drawn frame. The server
already composes into its own back buffer before copying to the
framebuffer.

Tests:
- `mouse_wheel`: injected four byte packets produce `dz` events, three
  byte devices keep working.
- `fb_format`: the kernel reports the mask layout and a user program
  draws a known color that the kernel verifies in the native format
  (run with a 24 bit mode in addition to the default).
- `gui_resize`: a client resizes and maximizes a window, the server log
  shows the new geometry and the CRC of the redrawn frame matches.
- `gui_wm`: Alt+Tab focus order, minimize to the task bar and restore
  by clicking the task bar button, occlusion (a window fully covered
  is not drawn, checked by a counter in the server log).
- `gui_clip`: two clients exchange text through the clipboard, checked
  through the client logs.
- `gui_term`: extended with a resize of the terminal window followed by
  a `stty size` style query in the shell and a scrollback check.
- Result: `startgui` shows a task bar with a launcher, resizable
  windows with maximize and minimize, a terminal that resizes and
  scrolls back, and text copied between applications.

## M20. Font rendering (completed 2026-08-30)

Decisions (2026-08-30): outline font rendering lives in its own library
`libfont/` (`libfont.a`), independent from the window server and the
toolkit; libgui consumes it through the existing `struct font` so every
widget and application can switch to an outline font without changes.
User programs still run without FPU state saving, so the rasterizer and
all transforms use fixed point integer arithmetic. Test fonts are the
DejaVu faces (TrueType outlines, `kern` and `GPOS` tables) and Latin
Modern Roman (CFF outlines, `GPOS` kerning) from `third_party/`.

Stage 1, parser (`libfont/src/ttf.c`, `cff.c`):
- `font_open(path)` maps the file, reads the table directory of TrueType
  (`\0\1\0\0`, `true`) and OpenType (`OTTO`) files, and the `head`,
  `hhea`, `hmtx`, `maxp`, `cmap` (formats 4 and 12, format 0 as a
  fallback), `loca`, `glyf`, `CFF `, `kern` and `GPOS` tables.
- Glyph lookup by Unicode code point; advance width and left side
  bearing in font units; ascent, descent, line gap, units per em.
- Outlines as contours of points in font units: TrueType quadratic
  curves from `glyf` including composite glyphs with offsets and
  scales; CFF Type 2 charstrings (moveto, lineto, curveto families,
  hstem/vstem/hintmask skipping, subroutines with bias, endchar,
  the width prefix) with cubic curves.

Stage 2, rasterizer (`libfont/src/raster.c`):
- Outlines are scaled to a pixel size in 26.6 fixed point, curves are
  flattened by fixed subdivision, and edges are rasterized into an 8 bit
  coverage bitmap with non zero winding: 4 sub scanlines per pixel row
  and exact horizontal coverage between crossings, accumulated in a per
  row buffer. Output is a glyph bitmap with bearing and advance in
  pixels (advance in 26.6 for subpixel positioning of the pen).
- A glyph cache keyed by font, size and glyph id keeps the last few
  hundred bitmaps.

Stage 3, kerning and shaping (`libfont/src/kern.c`):
- `kern` table format 0 pairs and `GPOS` pair adjustment (lookup type
  2, formats 1 and 2 with class definitions, through extension lookups)
  for the `kern` feature; `font_kern(font, left, right)` in font units.
- `font_shape(font, text, size)` walks a string, applies kerning to the
  pen, and returns glyph positions in 26.6 pixels.

Stage 4, integration (`libgui/`):
- `gfx_font_open_ttf(path, px)` returns a `struct font` backed by an
  outline font; `gfx_text_font` blends coverage bitmaps into the surface
  (alpha over a solid colour or the surface contents), `gfx_text_width_font`
  and `gfx_text_index_font` use shaped positions, so widgets, `term` and
  `view` work unchanged. `view` gains an "Outline font" menu entry, the
  launcher font list in `/etc/fonts/` holds the three test fonts.

Tests:
- `ttf`: `/bin/fonttest` parses both fonts and checks units per em,
  glyph counts, glyph ids, advances, kerning pairs (`AV` from `kern` and
  from `GPOS`, `To` from Latin Modern's `GPOS`), outline bounds of `A`,
  and rasterizes `A` at 32 pixels: the bitmap fits the scaled bounds,
  contains fully covered and partially covered pixels, and the sum of
  coverage matches the glyph's area within tolerance.
- `gui_ttf`: a window draws a string with the outline font; pixels are
  checked for intermediate colours (antialiasing) against a plain
  background and a CRC of the text row is logged.
- Result: applications render antialiased, kerned TrueType and CFF
  OpenType text at any size.

## M21. Application framework core (completed 2026-08-30)

Decisions (2026-08-30, from sixteen questions): a retained widget tree
with typed events delivered through signals; one application object
driving every window, timer and extra descriptor; boxes plus a grid
container with size hints; a theme structure with named colours and
metrics; per widget invalidation with clipped partial redraws; the
default interface font is DejaVu Sans at 14 pixels rendered by libfont
with a global scale factor; widgets such as tabs, split panes, tool
bars, status bars, combo boxes, spinners, sliders, progress bars, radio
buttons, tree views, tables, image views and icons; PNG images through
an own inflate; a text editor widget with undo, word wrap and syntax
colouring; drag and drop between applications through the server;
interfaces described in a declarative text file loaded at runtime;
menus, tooltips and popups as undecorated override windows; per window
alpha in the compositor; boot tests plus a host unit test build. The
work is split into M21 (core), M22 (widgets, images, editor) and M23
(interface files, popups, drag and drop, compositor alpha). The M19
toolkit (`gui/widgets.h`) stays available until every application has
moved to the framework in M22, then it is removed.

Stage 1, application object (`libgui/src/app.c`, `gui/app.h`):
- `app_create`, `app_run`, `app_quit`; windows registered with the
  application; one `poll` loop over the event queue, timers
  (`app_timer_add(ms, repeat, cb)`, ordered by deadline) and application
  descriptors (`app_watch_fd(fd, events, cb)`), so `term` no longer needs
  its own loop. Events are demultiplexed to windows by id; `WM_RESIZED`,
  focus and close are turned into signals on the window widget.
- Idle work: redraws happen once per loop iteration after all pending
  events are handled, so bursts of input cost one paint.

Stage 2, widget object model (`libgui/src/widget.c`, `gui/widget.h`):
- `struct widget` with a class pointer (`struct widget_class`: name,
  size, `measure`, `layout`, `paint`, `event`, `destroy`), a parent and
  children list, geometry, flags (visible, enabled, focusable, dirty),
  properties (`widget_set_int`, `widget_set_text`, `widget_get_*`) and
  an id string for lookup (`widget_find(root, "ok")`).
- Signals: `widget_connect(w, "clicked", handler, arg)` where each
  signal name has a fixed argument structure (`struct sig_click`,
  `struct sig_change`, `struct sig_key`, ...); handlers return whether
  the event is consumed; several handlers per signal run in order.
- Focus traversal with Tab and Shift+Tab, keyboard accelerators
  (`widget_set_accel(w, key, mods)`), mnemonics in captions.

Stage 3, layout (`libgui/src/layout.c`):
- Size hints: minimum, preferred and maximum width and height computed
  by `measure`, stretch factors, alignment inside the cell, margins.
- Containers: `box` (horizontal or vertical, spacing, padding) and
  `grid` (rows and columns, spans, per row and column stretch). Layout
  runs top down after measurement bottom up; only subtrees marked for
  relayout are recomputed.

Stage 4, theme and painting (`libgui/src/theme.c`, `paint.c`):
- `struct theme` with named colours (window, text, disabled text,
  field, selection, accent, borders, highlights) and metrics (padding,
  spacing, border width, corner radius, scroll bar width, font, font
  size); a default theme compiled in and `theme_set` at runtime;
  a global scale factor applied to metrics and font size.
- Painting through a `struct painter` bound to a surface with a clip
  rectangle stack and an origin, so widgets paint in local coordinates
  and cannot draw outside their area; primitives: fill, frame, line,
  text with the theme font, image blit, rounded rectangles, focus ring.
- Partial redraw: `widget_invalidate` marks a widget and its ancestors;
  the paint pass repaints only dirty widgets (and the descendants they
  overlap), unions their rectangles and sends one `gui_damage` per
  window.

Stage 5, core widgets on the new model (`libgui/src/widgets/*.c`):
- label, button, check box, radio button group, text field, list view,
  scroll bar, scroll area (viewport with two scroll bars), canvas,
  separator, box, grid, window. These replace the M19 equivalents in
  behaviour and add the signal interface.

Stage 6, host unit tests (`libgui/tests/`):
- `make check` builds libgui and libfont with the host compiler against
  a fake surface and a scripted event source (`tests/harness.c`), and
  runs unit tests of layout (measured sizes, grid spans, stretch),
  signals (order, consumption), partial redraw (which rectangles are
  damaged), focus traversal and text editing.

Tests:
- `make check` on the host as above.
- `gui_app`: a boot test client with two windows, a timer and a watched
  pipe descriptor; the kernel injects input and checks the log lines
  and pixels, including that a single change repaints only its widget
  (damage rectangles logged by the client).
- Existing GUI cases continue to pass with the M19 toolkit in place.
- Pipes gained a `poll` operation so watched pipe descriptors block
  correctly (`kernel/ipc/pipe.c`).
- Result: applications are built from a retained tree with signals, an
  application loop, themed painting and partial redraws. Design notes
  in `docs/design/framework.md`.

## M22. Widgets, images and the text editor (completed 2026-08-30)

Stage 1, images (`libgui/src/png.c`, `image.c`):
- A deflate decoder (stored, fixed and dynamic Huffman blocks), zlib
  framing with Adler-32, PNG chunks (IHDR, PLTE, tRNS, IDAT, IEND),
  filters (none, sub, up, average, Paeth), colour types grey, RGB,
  palette, grey with alpha and RGBA at 8 bits, interlacing rejected.
  `image_load(path)` returns an RGBA `struct image`; `painter_image`
  blends with alpha. Icons for the launcher, buttons and menus from
  `/usr/share/icons/*.png`, generated at build time from simple sources.

Stage 2, controls: combo box (popup list, editable option), spinner,
slider, progress bar, tabs, split pane (draggable divider), tool bar
with icon buttons, status bar with sections, tooltip text on any widget.

Stage 3, data views: `struct model` interfaces for lists, trees and
tables (row count, cell text, children, expansion); tree view with
expanders and indentation, table with resizable and sortable columns,
both virtualised over the model so large data is not copied; selection
signals.

Stage 4, text editor widget (`libgui/src/editor.c`): a line array with
per line strings, an undo and redo stack of edit operations, word wrap
mode, line numbers, search, a highlighter interface with C and shell
highlighters supplied, and the clipboard. `edit` gains a graphical
front end `gedit` built on it.

Stage 5, application migration: `term`, `files`, `view`, `clock`,
`paint`, `pong`, `widgettest` and the launcher move to the framework;
`gui/widgets.h` and the M19 toolkit files are removed.

Tests: `make check` extended with PNG decoding (a known image compared
pixel by pixel, filter and colour type coverage), the deflate decoder
against stored and dynamic blocks, model views (row virtualisation) and
editor operations (undo and redo, wrap positions). Boot tests
`gui_controls` (combo box, slider, tabs, split pane through injected
input) and `gui_editor` (typing, undo, wrap, highlighting colours in
pixels). Result: a complete widget set with images and an editor.

## Display server rework (M23 to M26)

Decisions (2026-08-30): the message queue based `wsrv` is replaced by
a Wayland inspired design of our own (own wire format, no `wl_` or
`xdg_` names): Unix domain sockets with descriptor passing as the
transport, shared memory pools with attach, commit and buffer release,
a 60 Hz frame clock with frame callbacks, toplevel and popup roles with
the panel as a separate layer client, server side decorations that a
client can decline, a data device model for clipboard and drag and
drop, keymaps and key codes sent to clients which translate them,
protocol definitions in XML with a Python scanner generating the C
marshalling code, a new compositor and client library with the
applications ported through the unchanged libgui framework API. The
former M23 entry (interface files, popups, drag and drop) is
superseded: popups and drag and drop are part of M25 and M26, the
`.ui` builder is an optional later milestone.

## M23. Kernel IPC and libc (completed 2026-08-30)

New syscalls, numbered after `SYS_uname 54` in
`kernel/include/syscall_nums.h`, entries in `kernel/syscall/table.c`,
prototypes in `kernel/include/syscall/syscalls.h`, implementations in
`kernel/syscall/sys_ipc.c` and `sys_fs.c`, libc stubs through
`syscallN` (`libc/include/minios/syscall.h`), new headers
`libc/include/sys/socket.h`, `sys/un.h`, `sys/eventfd.h`,
`sys/timerfd.h`, additions to `fcntl.h`, `unistd.h`, `sys/mman.h`.

1. Unix domain stream sockets (`kernel/ipc/socket.c`): `socket(AF_UNIX,
   SOCK_STREAM, flags)`, `socketpair`, `bind` and `connect` on an
   abstract name (a kernel table of listening names, `SOCK_NAME_MAX
   32`; no filesystem inode), `listen`, `accept`, `shutdown`. A
   connection is two 64 KiB rings (one per direction) with the bounce
   buffer discipline of `pipe.c`; reads and writes block unless the
   descriptor is non blocking (`-EAGAIN`); `poll` reports `POLLIN`,
   `POLLOUT` and the new `POLLHUP`; peer close gives end of file and
   `-EPIPE`.
2. Descriptor passing: `sendmsg` and `recvmsg` with `struct msghdr`,
   `struct iovec` and `SCM_RIGHTS` control messages (at most 16
   descriptors per message). Passed files are queued in the socket as
   (byte offset, file list) records and delivered with the first byte
   of their message, as on Linux; unread files are released when the
   socket closes.
3. `memfd_create(name, flags)` and `ftruncate`: an unnamed shm object
   (`kernel/ipc/shm.c` gains a constructor without a name and a grow
   operation) that is passed over sockets and mapped with `mmap`.
4. `eventfd(initval, flags)` (64 bit counter, read blocks on zero or
   returns `-EAGAIN`, write adds, `poll`) and `timerfd_create`,
   `timerfd_settime`, `timerfd_gettime` (`kernel/ipc/eventfd.c`,
   `timerfd.c`; expiration counter driven by the timer subsystem, read
   returns and clears it).
5. Descriptor flags: `struct file` gains `nonblock`; `struct fdtable`
   gains a close on exec bit map; `fcntl(F_GETFL, F_SETFL, F_GETFD,
   F_SETFD, F_DUPFD, F_DUPFD_CLOEXEC)`; `O_NONBLOCK` and `O_CLOEXEC`
   accepted by `open`, `socket`, `socketpair`, `pipe2`, `eventfd`,
   `timerfd_create`, `memfd_create`; `execve` closes marked
   descriptors. Non blocking mode honoured by sockets, pipes, eventfd,
   timerfd and message queues.
6. `poll`: limit raised to 64 descriptors, `POLLHUP` and `POLLERR`
   defined in `minios/abi.h`, and a positive timeout waits on the poll
   wait queue with a deadline instead of the current 5 ms busy loop
   (`poll_files` in `kernel/ipc/mqueue.c:240`).
7. FPU state: `struct thread` gains a 512 byte 16 byte aligned area;
   `fxsave` on switch out and `fxrstor` on switch in
   (`kernel/sched/mlfq.c` around `context_switch`), initial state from
   `fninit` and a default `MXCSR`, CR4 `OSFXSR` and `OSXMMEXCPT` set in
   `cpu_init`, the signal frame saves and restores the area, and the
   user flags in `toolchain.mk` drop `-mno-sse -mno-sse2 -mno-80387`
   (the kernel keeps them). libc `memcpy` may then use 16 byte moves.
8. Documents: `docs/design/sockets.md` (sockets, descriptor passing,
   memfd, eventfd, timerfd, descriptor flags) and an FPU section in
   `docs/design/scheduler.md`; lock ordering in `docs/design/locking.md`.

Tests (`tests/cases/`): `sockets` (`/bin/socktest`: socketpair echo,
abstract bind, listen, accept and connect across fork, descriptor
passing of a memfd and a pipe end with contents verified on the
receiving side, `POLLHUP` on peer close, `-EAGAIN` in non blocking
mode, `-EPIPE`), `evfd` (eventfd counter semantics, timerfd periodic
expirations counted against `uptime_ms`, both under `poll`), `fdflags`
(close on exec across `execve`, `F_DUPFD_CLOEXEC`, `O_NONBLOCK` on a
pipe), `fpu` (two user threads and a forked child doing SSE
arithmetic in loops while the other runs, results checked; a signal
handler clobbering SSE registers), plus the poll timeout accuracy in
the existing `mq` case.

## M24. Protocol library, scanner and compositor core (completed 2026-08-30)

1. Protocol definition `protocol/core.xml`: interfaces `display`
   (sync, get_registry, error and delete_id events), `registry` (global,
   global_remove, bind), `callback` (done), `compositor`
   (create_surface), `shm` (create_pool, format event), `shm_pool`
   (create_buffer, resize, destroy), `buffer` (release event, destroy),
   `surface` (attach, damage, frame, commit, set_opaque_region as a
   rectangle list, destroy), `output` (geometry and mode events).
   Argument types: int, uint, fixed (24.8), string, array, object,
   new_id, fd. Message header: object id (u32), opcode (u16), size
   (u16); client ids from 1, server ids from 0xff000000; descriptors
   travel in `SCM_RIGHTS` control messages in the order the fd
   arguments appear.
2. Scanner `tools/wscan/wscan.py` (Python, no dependencies) writes
   `libwire/generated/core-client.h/.c` and `core-server.h/.c`:
   per interface a request function set (client) or listener structure
   (server), event listener structures (client) or send functions
   (server), interface descriptors with argument signatures for the
   generic marshaller. Generated code is committed.
3. Library `libwire/` (`libwire.a`, also compiled on the host for unit
   tests): connection with output buffer and descriptor queue, flush,
   read and dispatch, client proxies with listeners and user data,
   server resources with dispatch tables and destruction callbacks, id
   allocation and `delete_id`, roundtrip through `display.sync`,
   protocol error reporting that closes the connection.
4. Compositor `user/compositor/`: `main.c` (event loop over the listen
   socket, client sockets, input descriptors and the frame timerfd),
   `client.c` (connections, registry, globals), `surface.c` (pending
   and current state, attach, damage in surface coordinates, commit,
   buffer release when a newer buffer is committed or the surface is
   hidden), `shm.c` (pools mapped from passed memfds, buffer format
   `XRGB8888` and `ARGB8888` with per surface alpha), `scene.c` (the
   scene of surfaces with z order, damage merging, occlusion culling
   and per rectangle composition carried over from `wsrv/compose.c`),
   `backend_fb.c` (framebuffer mapping and format conversion from
   `wsrv/compose.c`), `input.c` (mouse and keyboard reading from the
   devices, carried over from `wsrv/input.c`, raw at this stage).
   Composition runs once per 16 ms frame tick when damage exists;
   `callback.done` carries the frame time in ms.
5. `user/compositor` logs `comp: ...` lines for the tests (client
   connected, surface created, buffer attached, frame, buffer
   released, slow frame).
6. Document `docs/design/protocol.md` (wire format, object model,
   scanner, library) and `docs/design/compositor.md` (core state
   machine, frame clock, buffer lifecycle).

Tests: `make check` gains `libwire/tests/` on the host (marshal and
unmarshal every argument type, descriptor passing over a host
socketpair, id allocation, error paths); boot test `comp_core`
(`/bin/comptest core`: connects, binds globals, creates a pool with two
buffers, draws, attaches, damages, commits, waits for `callback.done`,
commits the second buffer and receives `buffer.release` for the first;
the kernel checks pixels on the framebuffer and the log).

## M25. Shell, seat, data device and panel (completed 2026-08-30)

1. `protocol/shell.xml`: `shell` (get_toplevel, get_popup,
   create_positioner, get_layer_surface), `shell_surface` base
   (configure event with serial, ack_configure), `toplevel` (set_title,
   set_app_id, set_min_size, set_max_size, move, resize with edges,
   set_maximized, unset_maximized, set_minimized, close event,
   configure with width, height and a state array), `popup` (grab,
   done event, reposition), `positioner` (anchor rectangle, gravity,
   offset, constraint adjustment), `layer_surface` (anchor edges,
   exclusive zone, size, keyboard interactivity), `decoration`
   (set_mode server or client, mode event), `toplevel_manager` for the
   panel (toplevel list events: title, app_id, state; requests
   activate, minimize, close).
2. `protocol/seat.xml`: `seat` (capabilities, get_pointer,
   get_keyboard, name), `pointer` (enter, leave, motion, button, axis,
   frame events; set_cursor with a surface and hotspot), `keyboard`
   (keymap event with a descriptor and size, enter with pressed keys,
   leave, key, modifiers, repeat_info). Serials on every input event
   are required by move, resize, popup grabs, set_cursor and data
   device operations.
3. Keymap: `/usr/share/keymaps/us.mkm`, a small binary table mapping
   key codes to symbols and characters for the plain, Shift, Ctrl and
   Alt levels, generated by `tools/genkeymap.py`; the compositor sends
   it once per keyboard as a memfd; libgui gains `gui/keymap.h` with
   loading and translation, and key repeat driven by `repeat_info`
   through an application timer.
4. `protocol/data.xml`: `data_device_manager`, `data_source` (offer
   mime, send with a descriptor, cancelled, dnd_finished),
   `data_offer` (offer events, receive with a descriptor, accept),
   `data_device` (set_selection, start_drag with a source, origin and
   icon surface; data_offer, enter, leave, motion, drop, selection
   events). Contents flow through pipes created by the receiver and
   passed to the source owner.
5. Compositor: `shell.c` (roles, configure and ack cycle, cascade
   placement, minimum and maximum sizes, maximize, minimize, close,
   popups with grabs and dismissal, layer surfaces with exclusive
   zones), `decor.c` (server side title bars, boxes, resize handles,
   move and resize drags, clamping; suppressed when a client chose
   client side decorations), `seat.c` (focus, enter and leave, pointer
   and keyboard delivery, serials, cursor surfaces with a default
   cursor image), `data.c` (selection and drag state, offers, transfer
   plumbing), keyboard shortcuts (Alt+Tab, Alt+F4, Alt+drag).
6. Panel `user/panel/`: a layer client anchored to the bottom with an
   exclusive zone, the launcher menu (`/etc/launcher`), the toplevel
   list from `toplevel_manager` with activate and minimize, the clock;
   it launches programs and reaps them. `startgui` starts the
   compositor, the panel and the requested client.
7. Document `docs/design/shell.md` (roles, configure protocol,
   decorations, seat, data device, panel).

Tests: boot tests `comp_shell` (toplevel configure and ack on a
resize started by the compositor's decoration drag, maximize, minimize
through the panel's manager protocol, popup dismissal on an outside
click), `comp_seat` (pointer enter, motion, button, axis with serials;
keymap sent and translated by the test client; modifiers; repeat
info), `comp_data` (clipboard between two test clients through a
passed pipe; drag and drop with enter, motion, drop and a received
payload), `comp_panel` (the panel lists a toplevel and launches
`clock`); host unit tests for the positioner constraint logic and the
keymap translation.

## M26. libgui port and application migration (completed 2026-08-30)

1. `libgui/src/client.c` rewritten on libwire: `gui_window` becomes a
   surface with a toplevel or popup role, an shm pool with two buffers
   (swap on commit, wait for release before reusing), damage
   accumulated by `gui_damage` and committed once per `app_step` when
   the previous frame callback has fired; server events become the
   framework's `struct gui_event` (renamed from `struct wmsg`; the
   `window_message` entry point keeps its shape); keyboard events
   translated with `gui/keymap.h`, key repeat from the application
   timer; `gui_set_title`, `gui_resize`, `gui_set_min_size` mapped to
   the toplevel requests; window resizes follow the configure and
   ack cycle (`WM_RESIZED` becomes the configure event, the framework
   relayouts and acknowledges on its next commit).
2. Popups: menus, combo boxes and tooltips become popup surfaces (M25
   `popup` with positioners) instead of floating widgets, so they can
   extend beyond their window; `window_popup_open` keeps its signature.
3. Clipboard and drag and drop: `gui_clipboard_set` and `get` over the
   data device; framework signals `drag_begin` (a widget starts a drag
   with text or file paths and an icon) and `drop` (`struct sig_drop`
   with mime type and contents); the editor, text field and file table
   support text and path drops.
4. `libgui` host tests: `fake_client.c` reimplemented over the new
   internal event structure; the existing framework tests keep passing.
5. Applications: `term`, `files`, `view`, `gedit`, `clock`, `paint`,
   `pong`, `mandel`, `widgettest`, `guitest`, `apptest`, `fonttest`
   unchanged except where they used `wmsg` fields directly; `wsrv`,
   `gui/proto.h`, the message queue transport and `wsrv.clip` removed;
   the `gui_*` boot tests updated to the compositor's log lines and
   the new geometry; `docs/design/gui.md` rewritten around the new
   stack, `docs/INTRODUCTION.md` updated.

Tests: all existing GUI cases under the new stack plus `gui_popup`
(a menu extends beyond its window and is dismissed by an outside
click on another window), `gui_dnd` (text dragged from `gedit` into a
second `gedit`), and `gui_frames` (a client that damages every frame
receives exactly one `done` per compositor frame, checked by counts).
