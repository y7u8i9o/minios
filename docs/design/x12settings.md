# x12settings

`x12settings` (`user/x12settings/`, installed as
`/usr/bin/x12settings`) shows the state of the running X12 and changes
its settings until X12 exits. The persistent choices of the user belong
to the Settings program. `x12settings set KEY VALUE` changes one setting
without a window. The plan of the tool is `docs/plan/x12settings.md`.

## Shared building blocks (X1)

The tool uses parts that other programs share:

- the process table of libc (`minios/proctab.h`, `libc.md`), which gives
  the program of a client,
- the graph widget, the colour dialog and the colour button of libgui
  (`framework.md`),
- the display modes of `gui/display.h`.

Before X1, `sysmon` drew its graphs with private code, seven programs
and tests parsed `/dev/proc` each in their own way, and X12,
`x12settings`, the desktop and the Settings program each contained the
packed display mode or the list of resolutions.

## Layout

The program consists of `main.c`, which connects to X12, binds the
`debug` interface of version 3 and the `settings` interface, dispatches
their events and builds the window, and of one file per page. The
window has the tabs Performance (`perf.c`), Clients (`clients.c`),
Settings (`live.c`) and Debug (`inspect.c`). A timer sends the
requests of the pages once per second. The line `x12settings: up ...`
reports the basic counters at each refresh.

## Frame history (X2)

`user/compositor/stats.c` records the last 1024 frames in a ring: the
serial, the end time in milliseconds since boot, the frame time, the
compose and the flush time in microseconds, the composed device pixels
and the longest latency that the frame ended. The serials count the
frames since the start of X12. A reset of the statistics does not clear
the ring. The request `get_frame_history(after)` of the `debug` interface
of version 3 sends a `frame` event for each recorded frame with a
larger serial, oldest first, and then `frame_history_done` with the
newest serial. `compstat -h` prints the ring.

## Performance page (X2)

Each second the page requests the frames after the newest serial it has
seen, the frame statistics and the basic counters. The first answer
gives only the newest serial, so the frames before the start of the
page do not count. The frames of a second give:

- the average and the longest frame time,
- the average and the longest latency of the frames that ended one,
- the composed device pixels.

The growth of the `wakeups` counter gives the wakeups of the second.
Four graph widgets show the last 60 seconds of these values with
automatic scales. A table below them lists the uptime, the clients,
the surfaces, the frame interval, the display, the resident memory of
X12 from the process table and every frame statistic with a readable
name. Times appear in milliseconds or seconds and byte counts in KiB or
MiB. The button "Reset statistics" sends `reset_frame_stats`. A second
with frames prints `x12settings: second of N frames, ...`.

The case `x12settings_perf` checks the history after 10 commits of
`comptest damage` with `compstat -h`, and the printed seconds while
`compbench anim` runs. Its qmp script takes a screendump of the page.

## Clients page

The debug interface of version 3 reports each client with its pid, uid,
number of surfaces and the bytes of its mapped shared memory pools
(`get_clients`). X12 takes the pid from the peer credentials of the
socket at connect time, and `shell_set_pid` of the client replaces it.
The page shows a tree of the clients, with the program from the process
table, and of their surfaces. Selecting a surface requests its details
(`get_surface`: buffer size, scale, transform, format, opaque rectangles,
commits), a thumbnail (`capture_surface` scales the current buffer into a
shared buffer of 240x160 pixels of the tool), and an outline in magenta
on the screen (`highlight`). The outline disappears when the selection
changes or the tool disconnects. `x12settings clients`, `x12settings
capture TITLE` and `x12settings highlight TITLE [SECONDS]` do the same
without a window.

## Settings page

Every setting of X12 has a control, which sends its value at once: the
display mode with its scale, following the host window, the frame
interval, key repeat, the keymap reload, pointer speed and
acceleration, the input method (the list comes from `get_input_methods`)
and its two switch keys, the decorations, the desktop colour through
the colour button, and logging. The controls follow the values that X12
reports, also after a change by another client.

## Debug page and views

The page switches the three debug views of X12 (`user/compositor/
overlay.c`), which are the setting keys `debug_damage`, `debug_opaque` and
`debug_fps`, and contains the pixel reader:

- `debug_damage`: each composed rectangle is tinted red for 300 ms. The
  flashes are recorded before the frame composes, so the frame draws
  them, and their expiry damages the rectangles again. Damage that the
  views add for themselves does not flash.
- `debug_opaque`: the opaque regions of the surfaces are tinted green.
- `debug_fps`: a box in the top right corner shows the frames of the last
  second. The views redraw it once per second when the number changes,
  and frames that only redraw the views do not count.

The case `x12settings_views` checks the client listing, the capture of a
`comptest` window, the outline and its removal at the exit of the tool,
the green tint, the frame counter and the red flash after a new desktop
colour. Its qmp script takes screendumps of the Clients and Settings
pages.
