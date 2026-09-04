# Debugging tools

The graphical debugging tools are ordinary framework applications in
`user/apps/`. They are listed in `/etc/launcher` and can be started from
a terminal as well. Each tool is built into `/bin`.

## sysmon

`sysmon` shows the process table. It reads `/dev/proc` once per second,
parses the `PID PPID PGID STATE TIME RSS NAME` rows without `sscanf` (the
libc has none), and lists them in a table with the CPU share of each
process (the growth of its `TIME` ticks over the refresh interval) and
its resident size. The label below the table shows the memory summary
from `/dev/meminfo`. The Terminate and Kill buttons send `SIGTERM` or
`SIGKILL` to the selected process. Since M41 a second tab holds the
sampling profiler: Start samples the selected process (or every process
without a selection) through `/dev/profile`, the table lists the hottest
symbols with their share, refreshed every second, the checkbox includes
or excludes kernel samples, and Stop freezes the result. User addresses
are symbolized with the `.symtab` of `/bin/<name>` of the sampled process,
kernel addresses with `/dev/ksyms` (see `profile.md`). Kernel samples
taken inside a spinlock section are attributed to the lock holder and
marked `(locked)` since M42.

## logview

`logview` follows the kernel log. The kernel keeps every `klog_print`
line in a 64 KiB ring (`kernel/lib/klog.c`) that `/dev/klog` exposes
(`kernel/fs/devfs.c`). A read returns the bytes after the file position,
`poll` reports readable data when the ring head has moved, and `lseek`
with `SEEK_END` skips to the current head. The ring lock is enabled by
`klog_ring_init` after the boot CPU exists, because the first log lines
are printed before any spinlock can be taken. The application watches
the descriptor with `app_watch_fd`, appends new lines to a read-only
editor, and offers a filter field, a pause button, and a clear button.

## hexview

`hexview` displays any file or device as a hexadecimal dump, sixteen
bytes per line with the printable characters at the right. The offset
field accepts a hexadecimal position, the scrollbar and the keyboard
move through the file, and Open prompts for another path. The default
path is `/dev/vda`, so the on-disk `mfs` structures can be inspected
while the system runs.

## evtest

`evtest` records the input events the framework delivers to a canvas
widget: pointer presses and releases with button state, wheel motion,
key presses and releases with key code, modifiers and translated
character, and window resize and focus changes. Every event is appended
to an editor inside the window and printed as `evtest: ...` on standard
output, so the tool also serves as a protocol check for the seat.

## x12settings

`x12settings` binds X12's `settings` and `debug` globals
(`protocol/debug.xml`). The Settings tab edits the frame interval, the
key repeat rate and delay, the default decoration mode, the verbose
flag and the desktop colour. Each change is sent with `settings.set`
and the compositor broadcasts the new value to every bound settings
resource, so several instances stay in agreement. The Surfaces tab
requests `debug.get_stats` and `debug.get_surfaces` once per second and
shows the composition counters and the surface list with role, title,
geometry, mapping state and buffer format. The command form
`x12settings set KEY VALUE` applies one setting and exits, which the
`gui_tools` boot test uses.

Settings keys: `frame_ms` (4 to 200), `desktop_color` (0xRRGGBB),
`repeat_rate` (1 to 100), `repeat_delay` (50 to 2000), `decorations`
(0 client, 1 server) and `verbose` (0 or 1).

## Boot test

`tests/cases/gui_tools` starts X12 and the panel, applies a setting
through `x12settings set`, starts each tool in turn, checks
that its window has the active title bar at its cascade position,
clicks inside `evtest`, closes every window with Alt+F4, and verifies
that each process exits with status 0.
