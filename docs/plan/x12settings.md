# x12settings: inspection and settings of the running X12

`x12settings` (`user/apps/compsettings.c`) changes the running display
server and shows its state. Before this plan it had four tabs: twelve
status values, a table of surfaces, six of the fifteen setting keys and
a pixel reader. This plan turns it into a full inspection tool. Each
milestone ends with boot tests in `tests/cases/` and is marked completed
here when they pass.

## 1. Scope

The owner chose the following scope on 2026-10-06:

- **Performance:** every frame statistic, graphs of the last minute and
  the memory of X12.
- **Clients and surfaces:** the clients with their programs and buffer
  memory, a tree of their surfaces, a detail view with a thumbnail, and
  an outline of the selected surface on the screen.
- **All live settings:** every server setting as a control.
- **Debug views:** overlays of X12 for damage, opaque regions and the
  frame rate.

## 2. Fixed decisions

- `x12settings` remains the tool for the running server. The Settings
  program remains the place of the persistent choices. A change in
  `x12settings` lasts until X12 exits.
- The window has the tabs Performance, Clients, Settings and Debug. The
  pixel reader moves to the Debug tab.
- The debug interface moves to version 3 (`protocol/debug.xml`). New
  requests and events:
  - `get_frame_history(after)`: one `frame` event per recorded frame
    with a serial above `after`, then `frame_history_done`.
  - `get_clients`: one `client_info` event per client (number, pid, uid,
    surfaces, mapped pool bytes, not responding), then `clients_done`.
  - `get_surface(id)`: one `surface_info` event with the buffer size,
    scale, transform, format, opaque region, parent, commits and frame
    callbacks.
  - `capture_surface(id, buffer)`: the current buffer of the surface,
    scaled into a shared memory buffer of the client, then
    `surface_captured` or `capture_failed`.
  - `highlight(id)`: X12 outlines the surface on the screen, 0 removes
    the outline.
- The debug views are setting keys of the existing settings interface:
  `debug_damage`, `debug_opaque` and `debug_fps`.
- Shared code goes into libc and libgui, and the private copies are
  replaced:
  - `minios/proctab.h` in libc reads the process table of `/dev/proc`.
    It replaces the parsers in `ps`, `sysmon`, `profiler`, `wireview`,
    `libprof` and the tests.
  - `graph_new` in libgui draws time series with grid, scale and
    legend. It replaces `draw_series` and its helpers in `sysmon`.
  - `color_dialog` in libgui chooses a colour by hue, saturation and
    value and by its hexadecimal value.
  - `gui_display_modes` in libgui lists the modes of a virtio-gpu
    scanout. It replaces the list in `user/settings/look.c`.
- `compstat` gains `-h` for the frame history and `-c` for the clients,
  so the boot tests can check the new events without reading pixels.

## 3. Milestones

### X1. Shared building blocks (completed 2026-10-06)

Built: `proc_table_read` and the replacements of the private parsers,
`graph_new` and the conversion of `sysmon`, `color_dialog`,
`gui_display_modes` and the conversion of the Settings program.

Tests: `make check-libgui` checks the scale of the graph and the colour
conversions. `libctest` checks the process table reader. The cases of
`sysmon`, `ps`, the profiler, `wireview` and the Settings program run
unchanged.

Document: `docs/design/x12settings.md` (new), `docs/design/libc.md`,
`docs/design/framework.md`.

### X2. The Performance page (about 650 lines)

Built: a ring of the last 1024 frames in `user/compositor/stats.c`
(serial, end time, frame time, compose time, flush time, composed
pixels, longest latency that the frame ended), debug version 3 with
`get_frame_history`, `compstat -h`, and the Performance page: graphs of
frame time, latency, composed pixels per second and wakeups per second
over the last minute, a table of every statistic, and a reset button.

Tests: the case `x12settings_perf` makes 20 commits with `comptest`
and requires 20 history entries from `compstat -h`, then opens the page
and checks that the frame time graph has drawn a line.

### X3. Clients and surfaces (about 1000 lines)

Built: buffer memory per client in X12, `get_clients`, `get_surface`,
`capture_surface`, `highlight` and the outline in the scene,
`compstat -c`, and the Clients page: a tree of clients and their
surfaces, a detail pane with a thumbnail, and the outline of the
selected surface.

Tests: the case `x12settings_clients` requires the pid, the program
and the pool bytes of two `comptest` windows from `compstat -c`, the
colour of a captured thumbnail and the outline pixels on the screen.

### X4. All live settings (about 450 lines)

Built: the Settings page with every setting key: display mode, follow
the host display, frame interval, key repeat, pointer speed and
acceleration, the input method and its keys, decorations, keymap
reload, logging, and the desktop colour through `color_dialog`. The
controls follow changes that other clients make.

Tests: the case `x12settings_settings` changes the pointer speed and
the desktop colour through the window with the keyboard and checks the
values with `x12settings get` and a screen pixel.

### X5. Debug views (about 500 lines)

Built: the overlay pass of the scene: damaged rectangles flash for 300
ms, opaque regions are tinted, and a frame counter in the top right
corner changes once per second. The Debug page switches them and
contains the pixel reader.

Tests: the case `comp_overlay` switches each view, makes a commit and
checks the tinted pixels, and then the cleared pixels after the flash.

## 4. Size

About 3400 changed lines with tests and documents: X1 800, X2 650, X3
1000, X4 450, X5 500.
