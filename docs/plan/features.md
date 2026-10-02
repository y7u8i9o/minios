# Work after the milestones

After M47 the work was organised by feature on `bleeding-edge-*` branches instead of numbered milestones. Each entry is recorded here when its boot tests pass.

## Debugging tools (completed 2026-08-30)

Graphical tools `sysmon`, `logview` (over `/dev/klog`), `hexview`, `evtest` and `compsettings` (over the `debug` and `settings` protocol interfaces). Documented in `docs/design/tools.md`, tested by `tests/cases/gui_tools`.

## Desktop and settings (completed 2026-08-30)

Desktop layer client with wallpaper, icons of `/home/desktop`, context menus, MIME tables (`gui/mime.h`), the user settings application `settings` and layer surface windows in libgui. Documented in `docs/design/desktop.md`, tested by `tests/cases/gui_desktop` and the libgui host test.

## Desktop look refresh (completed 2026-09-03)

Decorations moved to the GTK 4 model: libgui toplevels draw a light
header bar, outline, rounded corners and shadow in their own ARGB
buffers (`libgui/src/csd.c`), negotiate the mode through
the decoration interface and report their window geometry with the new
`toplevel.set_window_geometry` request; the server places, clamps,
maximizes and resizes by that geometry, copies opaque rows of ARGB
buffers and skips the desktop fill under opaque windows. The server
side decorations remain for clients without their own, redrawn in the
same style (title font, round buttons, soft shadow computed only in the
band around the frame). The panel draws through the libgui painter at
the output scale and libgui's rounded rectangles use circular corners.
Documented in
`docs/design/shell.md`, `docs/design/gui.md` and
`docs/design/protocol.md`; the decoration expectations of the `gui_*`
and `comp_*` cases were updated.

## Settings rework (completed 2026-09-04)

`/bin/settings` moved to `user/settings/` and became a category window
(Appearance, Display, Keyboard, Sound, Date and time, File types,
Launcher, System) that writes `/etc/desktop.conf` on every change; new
keys `frame_ms`, `decorations`, `keymap`, `ui_font`, `ui_font_px` and
`ui_scale` are applied by the desktop client, X12 (`keymap_reload`) and
libgui (`theme_read_conf`). `x12settings` shows the server's status,
surfaces, live settings and a pixel inspector. Documented in
`docs/design/desktop.md` and `docs/design/tools.md`, tested by
`tests/cases/gui_settings` (every page and the X12 tool open and close on
the desktop).

## Terminal and Files rework (completed 2026-09-05)

The terminal window was rebuilt on a separate emulator (`user/term/vt.c`):
UTF-8, 16, 256 and 24 bit colours with bold, faint, underline, reverse
and strike, a scrolling region, the alternate screen, cursor
visibility, bracketed paste, cursor position and device reports and OSC
titles; the window draws DejaVu Sans Mono antialiased at
`term_font_px` (13), has tabs, a scrollback bar, a mouse selection with
copy and paste, zoom keys and a context menu. The file manager
(`user/files/`) has a places list, a table with icons, size, type and
date columns, history, hidden files, new folder and file, rename,
delete with confirmation, copy, cut, paste, properties, Open with, Open
in terminal, a context menu and a listing that follows other programs.
The data views gained icons, a `context` signal, double click
activation and `view_select`; tabs can hide their title row. Documented
in `docs/design/terminal.md` and `docs/design/files.md`; tested by
`gui_term`, `gui_term_scale2` (updated to the 8x17 cells) and the new
`gui_files` case.

## Modification times (completed 2026-09-05)

`struct inode` carries `mtime`, reported by `stat` as `st_mtime`. mfs
reads and writes the field its disk inode already reserved, updated on
creation, writes and truncation and set by `mkfs` from the host files;
FAT decodes the entry's date and time; the initrd parses the tar
header; devfs dates its nodes from the boot. Tested by additions to
`mfs`, `fat` and `mfs_user`.

## Versioning (completed 2026-09-05)

Releases follow semantic versioning in `VERSION` (0.1.0) instead of the
milestone numbers; `tools/version.sh` numbers every kernel link in
`BUILDNUM` and records the commit and date, reported by `uname`, the
boot log and the System page. Documented in `docs/design/build.md`.

## X12 log file (completed 2026-09-05)

The display server logs to `/var/log/x12.log` instead of the serial
line; `-s` mirrors the log to standard output for the boot tests, and
the per frame, per key and per commit lines need the verbose setting
(`-v`). Documented in `docs/design/compositor.md`.

## SVG icons (completed 2026-09-05)

libgui renders a subset of SVG (`libgui/src/svg.c`: view box, paths
with fills, both fill rules, curves and arcs, antialiased) into images
that carry their device scale; `icon_get` prefers `<name>.svg` in
`/usr/share/icons` and keeps the PNG fallback. The icons are Font
Awesome Free (solid), downloaded by `tools/fetch_icons.sh` into
`third_party/fontawesome/`. Documented in `docs/design/icons.md`,
tested by `libgui/tests/test_svg.c`.

## sed and awk (completed 2026-09-06)

FreeBSD sed and the One True AWK are compiled unmodified from
`third_party/sed` and `third_party/awk` (downloaded by
`tools/fetch_sedawk.sh`) into `/bin/sed` and `/bin/awk`. The libc gained
`err.h`, `getopt`, the `scanf` family, `asprintf`, `wctype.h`,
`strings.h`, `libgen.h`, `limits.h`, `sys/uio.h`, `bsearch`, `random`,
`mbtowc`, `getprogname` and the permission bit macros, and `regexec`
accepts `REG_STARTEND` with `nmatch` 0. Documented in
`docs/design/sedawk.md`, tested by `tests/cases/sed`, `tests/cases/awk`
and the new checks in `tests/cases/libc_ext`.

## make (completed 2026-09-06)

pdpmake, the public domain POSIX make, is compiled unmodified from
`third_party/make` (downloaded by `tools/fetch_make.sh`) into
`/bin/make`. The kernel gained `utimensat` with the `setmtime` inode
operation in mfs and FAT, and `struct stat` carries `st_mtim`; the libc
gained `utimensat`, `strndup`, `stpcpy`, `realpath`, `access`, `confstr`
and `ar.h`; `touch` sets modification times. Documented in
`docs/design/make.md`, tested by `tests/cases/make` and new checks in
`tests/cases/libc_ext`.
Modification times are nanoseconds throughout the kernel and in the mfs
disk inode (format version 4), because pdpmake treats equal times as out
of date.

## ar and tar (completed 2026-09-06)

`ar` is written for minios (`user/coreutils/ar.c`) in the System V
format of the GNU binutils, and the tar of sbase is compiled unmodified
from `third_party/sbase` (downloaded by `tools/fetch_tar.sh`) into
`/bin/tar`. The kernel gained `openat` and `fstatat` (a directory opened
by name keeps its path in `file.path`) and `proc_reap_children` for the
run test; the libc gained `pwd.h`, `grp.h`, `sys/sysmacros.h`, `execlp`
and the refused `symlink`, `readlink`, `mknod` and `mkfifo`; `gzip`
accepts `-f`. Documented in `docs/design/artar.md`, tested by
`tests/cases/ar`, `tests/cases/tar` and new checks in `libc_ext`.

## Dynamic linking (completed 2026-09-06)

Programs are linked against shared objects in `/lib`: libc, libgui,
libfont, libwire, libaudio and the Lua core; `init` and the loader are
static. The kernel loads the loader named by `PT_INTERP` at
`USER_INTERP_BASE` and passes an auxiliary vector; `/lib/ld.so`
(`user/ld/`) maps the libraries, resolves the symbols and applies the
relocations before the program starts. Installed programs and libraries
are stripped of debugging information. `/dev/maps` lets the profiler
resolve addresses inside libraries. The root image of programs went from
62.6 MiB to 2.2 MiB plus 870 KiB of libraries. Documented in
`docs/design/dynlink.md`, tested by `tests/cases/dynlink` and the
regression of the existing cases.

## Host test portability and loader initialization (completed 2026-09-06)

Host checks use Darwin feature visibility and private strlcpy helpers.
The GUI fixtures and the text-field navigation handler use the input
core's KEY_* codes. `make check` and `make check-sh` pass on macOS.

The dynamic loader now supports GNU and SysV hashes, dependency-ordered
constructors and reverse-order destructors, absolute zero-valued symbols,
and a dynamically allocated object list. It validates ELF headers,
segments, dynamic tables, symbol indices and relocation destinations,
handles unaligned BSS-only segments and program headers beyond the first
page, defers COPY until other relocations finish, and protects GNU RELRO
pages. Libc initializes its runtime before calling constructors and uses
AT_BASE to distinguish loader callbacks from static exec startup.

The dynlink case includes a twenty-DSO chain, static startup, a RELRO
write check and 29 generated ELF fixtures. The targeted serial QEMU run
passed dynlink, libc, libc_ext, fork, pthreads, profile, shell2,
gui_widgets, gui_controls, gui_editor, gui_unicode, gui_lua and audio_server
(13/13). ELF TLS, dlopen, IFUNC, symbol versioning and lazy binding remain
unsupported. The implementation and startup ABI are in
`docs/design/dynlink.md`.

## Persistent storage (completed 2026-09-06)

The home directory lives on `data.img`, a data volume the build creates
once and never rebuilds, attached as `vdc` and mounted at boot by
`fsinit` from `/etc/fstab`, which seeds a fresh volume from
`/usr/share/skel/home`. The user's configuration moved to
`$HOME/.config/desktop.conf` over the shipped `/etc/desktop.conf`
(`conf_read_path`, `conf_write_path`). `mount` lists the mounted
filesystems, and `mkfs` includes dot files. Documented in
`docs/design/storage.md`, tested by `tests/cases/persist` with a post
script that reads the volume on the host.

## tcc (completed 2026-09-06)

The Tiny C Compiler is compiled unmodified from the submodule
`third_party/tinycc` into `/bin/tcc`, with its runtime library built by
the cross compiler, the libc headers installed under `/usr/include`, the
C runtime objects under `/lib` and the loader as the ELF interpreter, so
that programs compile, link and run on minios; `-run` works through the
new `dlfcn.h` of libc. Documented in `docs/design/tcc.md`, tested by
`tests/cases/tcc` with 54 programs of the upstream suite.

## Code editor (completed 2026-09-06)

A source editor for C, Lua and shell scripts written in Lua
(`user/share/apps/code.lua`, started by `/bin/code`), with a run panel
over `sys.spawn_pipe`, an outline table and languages as table entries
that `$HOME/.config/code.lua` may extend. The `gui` module gained the
editor, menu, tool bar, status bar, icon and data view bindings with Lua
models; the toolkit gained `highlight_lang`, a highlighter driven by a
language description, with Lua as a third language. Documented in
`docs/design/code.md`, tested by `make check-lua` and
`tests/cases/gui_code`.

## Terminal userland (completed 2026-09-06)

The work of `terminal.md`. `libedit/` is a line editor with
history, completion, reverse search and bracketed paste. `/bin/sh` was
rewritten as a parser of complete command trees (`if`, `for`, `while`,
`until`, `case`, functions, subshells, brace groups) with POSIX expansion
order, here documents, `local`, `alias`, `source` and startup files
(`/etc/profile`, `$HOME/.shrc`). The framebuffer console keeps 16 colour
SGR attributes per cell and preserves the order of user output across
CPUs. libc gained `glob`, `fnmatch`, `wcwidth`, `getline` and the `term.h`
helpers; `ls`, `grep`, `less`, `tree` and `df` use colour and the terminal
width. Documented in `docs/design/libedit.md` and `docs/design/sh.md`,
tested by `lineedit`, `lineedit_screen`, `script2`, `console_sgr` and
`libc_ext`.

## Lua (completed 2026-09-06)

Lua 5.5.1 built unmodified from `third_party/lua` into `/bin/lua` and
`/bin/luac`, with the modules `fs`, `sys` and `gui` in `user/lua/` and the
clock and pong programs in Lua. libc gained the stdio, process and
`setjmp` functions the interpreter needs and a binned heap allocator.
Documented in `docs/design/lua.md`, tested by `lua`, `lua_conf`,
`lua_gc`, `lua_sys`, `gui_lua` and `make check-lua`.

## Packages (completed 2026-09-06)

`pkg` (`user/pkg/`) installs, lists, verifies, removes and builds
`.mpk` archives under the prefix `/home/.local`, which lies on the data
volume; the loader searches `/home/.local/lib` after `/lib`. The desktop
applications of `user/packages/` are built into `/usr/share/packages`;
Code and Pong ship as packages since 2026-09-15 and `man` reads the
pages of installed packages. Documented in `docs/design/packages.md`,
tested by `pkg` and `pkg_apps`.

## TCP/IP (completed 2026-09-12)

The network stack of `network.md`, milestones N00 to N12: packet
buffers, the worker, virtio-net, Ethernet, ARP, IPv4 with fragments and
path MTU, ICMP, UDP, TCP with congestion control, DHCP, DNS, the socket
ABI and the tools `net`, `dhcpc`, `ping`, `nc`, `http` and `xfer`.
Documented in `docs/design/network.md` and the validation records
beside it, tested by the `net_*` cases.

## Thread local storage, dlopen and lazy binding (completed 2026-09-15)

The loader lays out the TLS blocks of the initial objects below every
thread control block (variant II), applies the `DTPMOD64`, `DTPOFF64`
and `TPOFF64` relocations, and serves `__tls_get_addr` for objects
loaded later through a dynamic thread vector per thread. `dlopen`,
`dlsym`, `dlclose` and `dlerror` load, search and unload shared objects
at run time with reference counts, local and global scopes and
finalizers in reverse order; objects linked without `-z now` bind their
procedure linkage table entries on first call through
`_dl_runtime_resolve`. The C library and the loader share the record in
`minios/dl.h`; a static program handles its own TLS segment. Documented
in `docs/design/dynlink.md`, tested by `tests/cases/dlopen` and the
extended `dynlink` fixtures.

## Init (completed 2026-09-15)

Process 1 reads `/etc/init.conf`: `env` lines, `task` entries run in
order, `service` entries supervised with restart limits, and the
`console` session, with `if=`, `log=` and `restart=` options and a
built-in fallback table. `initctl` lists, starts, stops, restarts and
reloads entries and requests the shutdown over an abstract Unix socket.
The audio server and the DHCP client (`dhcpc -a`) became services of
the shipped configuration.
`sys_reboot` flushes the console and releases init's mappings after an
RCU grace period so the root filesystem unmounts cleanly. Documented in
`docs/design/init.md`, tested by `tests/cases/initctl`, `shutdown_cmd`
and `shutdown`.

## Lua threads and the Lua synthesizer (completed 2026-09-15)

`require "thread"` starts native worker threads, each with its own Lua
state, that exchange byte strings with the parent; `sys` gained
descriptor and timing functions and `require "audio"` binds `libaudio`.
The Lua Synthesizer (`luasynth`) is an eight voice instrument written in
Lua and shipped as a package, with its synthesis on a worker thread.
Documented in `docs/design/lua-threads.md`, `docs/design/luasynth.md`
and `docs/design/luasynth-performance.md`, tested by `lua_threads`,
`lua_audio`, `luasynth`, `luasynth_worker`, `luasynth_profile` and
`gui_luasynth`.

## Profiler application (completed 2026-09-15)

`/dev/profile` records CPU samples, scheduler transitions, heap
allocations and transfers in per CPU rings; `kernel/debug/unwind.c`
stitches kernel stacks onto the user frames that caused them. The
analysis library in libc (`profanalyze.c`, `profreport.c`) builds call
trees and reports, `prof` prints them, and `/bin/profiler` shows a flame
graph with breakdown, search and capture controls. Documented in
`docs/design/profile.md`, tested by `profile`, `profreport`, `prof_gui`
and `profiler_gui`.

## Boot log (completed 2026-09-30)

`timer_early_init` calibrates the TSC first thing in `kmain`, and every
kernel log line carries the time since the kernel entry. The boot log
states the bootloader, the image and initrd placement, the CPU, the
memory map and allocator counts, the interrupt controllers, the timer
rates, each PCI function by name, the loader of each process, `/dev` in
one line and the boot time. Init logs in the same format and reports
configuration reads, starts, exits, restarts and the startup summary.
Documented in `docs/design/console.md` and `docs/design/init.md`, tested
by `boot`, `timer` and the init cases.

## Lua images, clipboard, layer windows and prompt (completed 2026-09-30)

The `gui` module gained images (`user/lua/limage.c`): `gui.image` loads
PNG files and SVG files rendered at the output's scale, `gui.from_pixels`
builds one from a string, images have `size`, `scale`, `pixel` and
`pixels`, the painter draws them at any size through a cached box
filtered rendition, `gui.imageview` shows one fitted to its area and
`widget:image` gives labels and buttons one. The application gained
`clipboard([text])` over the libgui data device calls, `layer(w, h,
options)` for layer surface windows and `screen()`. The interactive
prompt edits lines with libedit through the readline hooks of `lua.c`
(`user/lua/lreadline.c`), with Tab completion of Lua names and a history
file; `lua.1` and `luac.1` are plain text manual pages, and the image
has the module directory `/usr/share/lua/5.5`. X12 detaches the offers
of a destroyed source, which fixed a use after free found by the new
test. Documented in `docs/design/lua.md` and `docs/design/shell.md`,
tested by `make check-lua`, `gui_lua_bindings` and `lua_prompt`.

## Symbolic links (completed 2026-09-30)

The VFS resolves symbolic links in every component of a path, and in the
last one unless the caller asks for the link itself, with relative
targets taken from the directory holding the link, at most 40 links per
lookup and `..` resolved against the directory a link led to; the working
directory and the paths of open directories are kept without links.
`open` refuses a link with `O_NOFOLLOW` and creates the target of a
dangling link with `O_CREAT`. The system calls `symlink`, `symlinkat`,
`readlink`, `readlinkat` and `lstat` were added and `fstatat` and
`utimensat` honour `AT_SYMLINK_NOFOLLOW`. mfs stores the target in a
journaled data block without a format change, `mkfs` copies host links
and `fsck` checks them, the initrd reader accepts typeflag `2`, and FAT
and devfs refuse links with `EPERM`. libc gained the calls and a link
aware `realpath`; `ln -s`, the new `readlink`, `ls`, `stat`, `find`,
`cp`, `rm`, `du`, `tree`, the Files program and the sbase `tar` handle
links. The design is described in `docs/design/vfs.md`,
`docs/design/mfs.md`, `docs/design/fat.md` and `docs/design/syscall.md`,
and the boot test `symlink` checks it.

## Signed package repositories (completed 2026-09-30)

`pkg` installs from repositories served over plain HTTP. A repository's
index lists every archive with its name, version, dependencies, size and
SHA-256 digest and is signed with Ed25519. `pkg update`, `search`,
`install NAME[-VERSION]` and `upgrade` accept an index only when its
signature verifies against a key in `/etc/pkg/keys/` and an archive only
when it matches its entry, resolve `depends` and `needs` through the
index, and `pkg check` compares a local archive with the index.
SHA-256, SHA-512 and Ed25519 are written in `libc/src/crypto/` from RFC
6234 and RFC 8032, the client of `http` moved to `libc/src/net/http.c`
with timeouts and `Content-Length` checks, and the host tool
`tools/pkgsign` with `tools/mkrepo.sh` generates the build's key under
`build/pkg/` and signs the repository that `make repo` writes to
`build/repo/`. Documented in `docs/design/packages.md`,
`docs/design/network.md` and `docs/design/build.md`, tested by
`pkg_repo` and `make check-pkg`, with `pkg`, `pkg_apps` and `net_tools`
as regressions.

## TCP options, resolver cache and DHCP robustness (completed 2026-09-30)

Milestones N13 to N16 of `network.md` closed the limitations that
the network release carried. They added TCP window scaling, timestamps with
PAWS and round-trip samples, selective acknowledgements with a bounded
scoreboard and RFC 6675 recovery, and delayed ACKs; per-connection stores of 64 KiB
for sending and 128 KiB for receiving, released as soon as no endpoint
needs them; a resolver cache per process with TTLs bounded to one hour,
negative caching after RFC 2308 and the search list of
`/etc/resolv.conf`; and RFC 5227 conflict detection in `dhcpc` through a
new `/dev/net` probe operation, with DHCPDECLINE, announcements and a
lease kept on the home volume for INIT-REBOOT. `netpeer` gained a scripted
TCP peer and `check_capture.py` checks option use on the wire. Documented
in `docs/design/network.md` and `docs/design/network-n13-n16-validation.md`,
tested by `net_tcp_options`, `net_tcp_options_peer`, `net_tcp_sack`,
`net_dns_cache`, `net_arp_probe` and the extended `net_dhcp`, `net_dns` and
`net_tools` cases.

## Protocol viewer (completed 2026-09-30)

libwire's server calls an optional trace hook for every request it
decodes and every event it queues, and `wire_format_args` writes the
arguments of a message as text. X12 offers the `tracer` interface of
`protocol/debug.xml`, which streams the traffic of every other client
as `message` events and reports clients, their pids and the messages
it had to drop for a slow tracer. `wireview` lists the traffic with
filters by client, text and frame traffic, shows the history of the
selected object and the message rate of every client over the last 30
seconds, and prints the trace in text mode with `-t`. Documented in
`docs/design/protocol.md`, `docs/design/compositor.md` and
`docs/design/tools.md`, tested by `make check` for libwire and the boot
test `gui_wireview`.

## Hung task detector (completed 2026-10-02)

- `/dev/threads` lists every thread with its state, wait queue and kernel
  frames.
- A kernel thread reports waits for a sleeping lock or a block request
  that last longer than a limit, with the thread table.
- Alt+SysRq prints the thread table on the console.
- The boot test is `hung_task`.

`/dev/threads` located the terminal fault of 2026-10-02 (`locking.md`),
which the profiler could not show, because it records a stack only when a
block ends. `docs/design/debug.md` describes the detector.

## Images, screenshots, image viewer and paint (completed 2026-10-02)

- The PNG decoder reads every bit depth and Adam7 interlacing. libgui
  encodes PNG files (`image_encode_png`, `image_save_png`), resamples
  images (`image_scale`) and draws them scaled (`painter_image_scaled`).
- The `screencopy` interface of X12 copies the screen into a client
  buffer. `/bin/screenshot` saves it as a PNG file, and Print Screen
  starts it.
- `view` became an image viewer for PNG and SVG files with zoom, scrolling,
  navigation through the directory and Set as wallpaper. The text viewer
  was removed, because gedit opens text files.
- `paint` saves and opens PNG files and has a brush, an eraser, lines,
  rectangles, ellipses, flood fill, a palette, undo and redo.

Documented in `docs/design/images.md`, tested by `make check` for libgui
and the boot test `gui_images`.

## Hex viewer update (completed 2026-10-02)

`hexview` shows as many lines as its window has room for, in DejaVu Sans
Mono, and has a byte cursor with a selection, a value inspector, search
for text and for bytes, a go to offset field, copying of the selection,
menus and a text size setting. Documented in `docs/design/tools.md`,
tested by the boot test `gui_hexview`.

## Kernel log viewer update (completed 2026-10-02)

`logview` shows the kernel log in a table with time, level, subsystem and
message, filters by level, subsystem and text, follows new rows, shows the
selected line in full, and copies and saves lines. `/dev/klog` accepts
`SEEK_CUR`, which logview uses to count bytes that the ring dropped.
libgui has `view_scroll_to` and `view_scroll_position`. Documented in
`docs/design/tools.md`, tested by the boot test
`gui_logview` and the libgui host test.

## System monitor update (completed 2026-10-02)

`sysmon` has a Processes tab with a sortable process table, a name filter,
a table of the threads of the selected process and a context menu, and a
Resources tab with graphs of the usage of each CPU, of memory and swap and
of the network rates over 60 seconds. The kernel counts the user, system
and idle ticks of every CPU and lists them in `/dev/cpustat`. Documented
in `docs/design/tools.md` and `docs/design/debug.md`, tested by the boot
test `gui_sysmon`.

## Launcher update (completed 2026-10-02)

The launcher menu of the panel has a search field with keyboard
selection, the headings Applications and System, an icon for every entry,
a separate Log out row, and two columns when one column does not fit on
the screen. The launcher code moved from `user/panel/panel.c` into
`user/panel/launcher.c`. Documented in `docs/design/shell.md`, tested by
the boot tests `comp_panel` and `gui_wm`.

## Editor update (completed 2026-10-02)

The editor widget has word movement and deletion, automatic indentation,
indentation of selected lines, word and line selection by double and
triple click, undo groups, `editor_replace_all` and public clipboard
functions. Editors and text fields have a context menu with the editing
commands. Menus show accelerators, and disabled icons are dimmed. gedit
has a full Edit menu, Replace, Go to line, Lua highlighting and a status
bar with the language and the line count. Documented in
`docs/design/framework.md`, tested by `make check` for libgui and the
boot test `gui_editor`.
