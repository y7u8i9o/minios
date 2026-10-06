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
window has the tabs Performance (`perf.c`), Surfaces (`clients.c`),
Settings (`live.c`) and Inspector (`inspect.c`). A timer sends the
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
