# User space applications

M16 completes the user environment: the shell, the coreutils, a screen
editor and a scripting interpreter, together with the kernel support
they need (raw keyboard mode, console cursor control, a sleep call).

## Kernel support

- Terminal modes. The console line discipline in `arch/x86_64/ps2kbd.c` has
  the flags `ICANON`, `ECHO` and `ISIG` (`minios/abi.h`), read and set
  through `ioctl` on `/dev/console` with `TCGETS` and `TCSETS`;
  `TIOCGWINSZ` reports the text size. Without `ICANON` every byte is
  delivered as soon as it is typed and the cursor keys (arrows, Home,
  End, PageUp, PageDown, Delete) arrive as VT100 escape sequences; the
  Escape key itself sends `ESC`. Leaving canonical mode releases a
  partially typed line. Control C sends `SIGINT` and control Z sends
  `SIGTSTP` while `ISIG` is set. libc provides `tcgetattr` and `tcsetattr` (`termios.h`) and
  `ioctl` (`sys/ioctl.h`). Since U5 of the multiuser plan the terminal
  stores the whole `struct termios` with the flag values of Linux, and
  `TCFLSH` and `TCSAFLUSH` discard the typed input (`users.md`).
- Console escapes. `drivers/fbcon.c` parses CSI sequences: `ESC[row;colH`
  (cursor position), `ESC[2J` (clear screen), `ESC[J` (clear to the end
  of the screen), `ESC[K` (clear to the end of the line) and `ESC[nA` to
  `ESC[nD` (cursor movement). Unknown sequences are ignored. The serial
  console passes the bytes through to the host terminal.
- `sleep_ms(ms)` blocks the calling thread until its deadline or a signal.
  libc exposes `sleep`, `usleep` and `sleep_ms`.
- `wait4` accepts `WNOHANG`, `WUNTRACED`, `WCONTINUED`, and negative pids
  for process groups. Stop and continue events do not reap the child.
- `SIGHUP` to init requests `RB_HALT`.

## Shell

The tree parser, expansion, builtins, startup and job control are described
in [Shell](sh.md). Interactive history, completion and key handling are implemented in
the reusable [line editor](libedit.md).

## Coreutils

`cat` (`-n -b -s -A`), `cp`, `clear`, `echo`, `halt`, `head` (`-n -c`), `hexdump`, `kill`
(`-SIG`), `ls` (`-1 -C -l -h -a -t -r -S -d -F`), `mkdir`, `mount` (`mount type source target`,
`mount -u target`), `mv`, `ps`, `pwd`, `reboot`, `rm` (`-d`), `rmdir`,
`shutdown` (`-r`), `sleep` (fractional seconds), `sync`, `touch`, `wc`
(`-l -w -c`). `shutdown`, `reboot` and `halt` signal init with
`SIGUSR1`, `SIGUSR2` and `SIGHUP`.

Added after M18, all in `user/coreutils/`:

- Text: `grep` (BRE by default, `-E -F -i -n -v -c -l -r -h -H -w`), `tail` (`-n -c -f`), `sort`
  (`-r -n -u`), `uniq` (`-c -d`), `tr` (ranges, escapes, `-d -s`), `cut`
  (`-d -f`, `-c`), `rev`, `nl`, `tee` (`-a`), `seq`, `yes`, `printf`,
  `cmp`, `diff` (longest common subsequence, `<`/`>` output), `more`
  (forward pager), `less` (`-R -N -S`, regex search, paging and percentage
  jumps), `pager` (compatibility wrapper for `less`), `tree` (`-L -a -d`),
  `find` (name, type and depth tests, NUL output and `-exec`),
  `xargs` (quoted and NUL input, batching and replacement), `gzip`
  (interoperable DEFLATE compression/decompression with CRC checking),
  and `man` (section lookup, whatis/apropos search and installed pages),
  `banner` (letters rendered from the GUI font).
- System: `env`, `printenv`, `test` (string, integer and file tests,
  `!`), `expr` (integer arithmetic and comparisons), `which`, `stat`,
  `ln`, `du` (`-s`), `free` (`/dev/meminfo`), `uptime` (time since boot
  and CPU count), `nproc`, `uname` (`-a -s -n -r -v -m`, from the `uname`
  syscall; the release number follows the milestone), `basename`,
  `dirname`, `df` (filesystem capacity from `/dev/mounts`). `ps` aligns
  the process fields from `/dev/proc`, retaining spaces in process names.
- Amusements: `fortune` (entries in `/etc/fortunes` separated by `%`
  lines), `cowsay`, `sl` (a locomotive crosses the terminal), `matrix`
  (character rain until a key is pressed), `life` (`-g -w -h -q -r`),
  `mandel` (text mode of the plotter below, used without a window
  server or as `mandel columns rows`), `maze` (`maze width
  height [seed]`), `snake` and `2048` (raw mode, arrow keys or wasd, q
  quits).

The terminal programs use `TIOCGWINSZ` for the screen size, turn off
`ICANON` and `ECHO` for keys, and use `poll` on standard input for
timed input. `term.h` centralizes width lookup and colour gating: output
must be a tty, TERM must be set and not dumb, and NO_COLOR must be unset.
`ls` and `tree` colour names by type; `grep` colours matching spans.
User space has SSE2 floating point support; AVX remains disabled.

GUI programs in `user/apps/`, started from the terminal window: `clock`,
`view`, `unicode` (a Unicode code-point grid described below),
`paint` (mouse drawing, keys
1 to 7 pick a color, `+` and `-` change the brush, `c` clears), `pong`
(left paddle `w`/`s`, right paddle arrow keys, Escape quits) and `mandel`
(the Mandelbrot set on the application framework, see below).

`unicode` browses the code space with one font at a time, chosen from a
toolbar list of the installed fonts and defaulting to Unifont. A cell is
drawn only when `font_glyph_index` returns a non-zero glyph for its code
point; cells without a glyph keep the window background and carry their
hexadecimal label alone, so the grid shows the repertoire of the selected
font instead of a row of empty boxes. No fallback font is installed on
the grid font, because a fallback would fill those cells from another
font and misreport the coverage. The "Covered only" checkbox switches the
grid between all 1114112 code points and the covered ones packed
together; for the second mode the program walks the code space once per
font and stores the covered set as contiguous runs, each run recording
how many covered code points precede it, so a cell index becomes a
binary search. Selection moves with the arrow and page keys, the mouse,
the scroll bar and a hexadecimal entry field. The panel above the grid
shows the code point at 64 pixels, its block name from the table
generated in `user/apps/unicode_blocks.h`, its UTF-8 bytes and its glyph
id in the selected font.

`mandel` shows the Mandelbrot set and its Julia sets in double precision
arithmetic. The view consists of a centre and a scale in units per pixel.
The magnification ranges from 1/16 to 2^44 of the home view, which spans
3.2 units over 640 pixels. The automatic iteration limit is 160 plus 64
for each doubling of the magnification, and the Iterations list of the
tool bar selects a fixed limit of 256, 1024 or 4096 instead. Points in
the main cardioid and in the period 2 bulb are recognised without
iteration. An escaping point has the smooth iteration count
n + 1 - log2(log |z|) with the escape radius 256, and the colour is
interpolated between two entries of one of four palettes of 256 colours
(Classic, Fire, Ocean, Grey). The points of the set are black.

The program starts one worker thread per CPU, at most eight. The image is
divided into tiles of 64 pixels, and the workers take the tiles in the
order of their distance from the centre of the canvas. A worker renders
its tile in passes with the block sizes 16, 8, 4, 2 and 1. The first pass
samples the grid and fills 16 pixel blocks. Each later pass refines every
block of the previous pass: it samples the midpoints of the edges, and
when all eight boundary samples are inside the set, it marks the block as
inside without sampling its interior. Otherwise it samples the centre,
and the next pass splits the block. A worker samples only pixels of its
own tile, and the tiles share no data. The workers write each pixel of
the colour image with one relaxed atomic store, and the main thread
draws the image every 40 ms while a render runs. A mutex protects the
render job, the next tile, the number of finished tiles and the number
of active workers. A change of the view sets a cancel flag, waits until
no worker is active and starts a new job. A change of the palette after a
finished render recolours the image from the stored smooth counts
without a new render.

A left click or wheel up zooms in by a factor of 2 at the pointer, and
wheel down or a right click zooms out. A left drag pans: the image is
drawn shifted while the button is pressed, and on release the known part of
the image is moved and the rest is rendered. A right drag draws a
rectangle and zooms into it. The arrow keys pan by a quarter of the
canvas, + and - zoom around the centre, r resets the view and j or the
View menu switch between the Mandelbrot set and the Julia set of the
current centre, with the Mandelbrot view restored on return. Save image
(Ctrl+S) writes the canvas as a PNG file with `image_save_png`. The
status bar shows the centre or the Julia constant, the point under the
pointer, the magnification, the iteration limit and the progress or the
duration of the last render. Serial lines `mandel: N worker threads`,
`mandel: rendering WxH at zoom level N` and
`mandel: render complete in T ms` mark the start and each render. The
boot test `gui_mandel` checks the colours of the home view, zooms with
the wheel and with a rectangle, shows a Julia set and saves a PNG file.

`/etc/tests/utils.sh` exercises the utilities; the `utils` case runs it
and rejects any line starting with `FAIL`. The `games` case
(`kernel/tests/test_games.c`) starts `snake`, `2048` and `matrix` through
the shell with their output redirected to files, waits until each has
switched the console to raw mode, feeds arrow keys and `q` through the
keyboard driver, and checks the final output and that canonical mode is
restored; it also runs `sl` and `life` to completion.

## sed and awk

`/bin/sed` (FreeBSD sed) and `/bin/awk` (the One True AWK) are compiled
unmodified from `third_party/`; see [sed and awk](sedawk.md) for the
build, the libc additions, the differences from the GNU programs and the
tests.

## make

`/bin/make` is pdpmake, the public domain POSIX make, compiled unmodified
from `third_party/make`; see [make](make.md). `touch` sets the
modification time of existing files since the same change.

## tcc

`/bin/tcc` is the Tiny C Compiler, compiled unmodified from the
submodule `third_party/tinycc`; `ld` and `as` run it under the
conventional names. See [tcc](tcc.md).

## init

Process 1 reads `/etc/init.conf`, runs the boot tasks, supervises the
services and the console session and answers `initctl`; see
[init](init.md).

## fsinit and mount

`fsinit` mounts the entries of `/etc/fstab` at boot and seeds a fresh
volume; `init` runs it as the first task. `mount` without arguments lists
the mounted filesystems. See [persistent storage](storage.md).

## ar and tar

`ar` (`user/coreutils/ar.c`) maintains System V archives and `/bin/tar`
is the tar of sbase, compiled unmodified from `third_party/sbase`; see
[ar and tar](artar.md). `gzip` accepts `-f` since the same change. The
gzip codec is part of libc (`minios/gzip.h`: `gzip_compress`,
`gzip_decompress`, `gzip_crc32`) since the package installer
([packages.md](packages.md)) uses it; `mkdir` accepts `-p`.

## Editor

`edit file` (`user/edit/edit.c`) is a nano style editor. It loads the
file into an array of lines, switches the console to raw mode without
echo, and redraws the screen with cursor positioning escapes after every
key. Arrows, Home, End, PageUp and PageDown move; printable characters
and Tab insert; Backspace and Delete erase and join lines; Enter splits
a line. Control S writes the file, control Q quits (twice when there are
unsaved changes), control L redraws. The last row shows the file name,
line count, cursor position and the last message. The terminal mode is
restored at exit through `atexit`.

## Interpreter

The first scripting language was a custom one, `mint` (`user/mint/mint.c`),
written when user programs had neither floating point nor `setjmp`. Both
exist now and Lua 5.5 is built from `third_party/lua/` as `/bin/lua`;
see [Lua](lua.md). mint stays for its test and as a small example of an
interpreter. mint has 64 bit integers and strings, arithmetic and
comparison operators, `&&`, `||`, `!`, string concatenation with `+`,
`if`/`elif`/`else`, `while`, `for x in range(a, b)`, `break`,
`continue`, functions with `fn name(params) { .. }` and `return`, and
the builtins `print`, `len`, `str`, `int`, `substr`, `ord`, `chr`,
`readfile`, `writefile`, `input`, `exit`, `argc`, `argv`. Assignments
inside a function bind locally unless the name is already global. The
implementation is a recursive descent parser producing a tree that is
evaluated directly; errors report the line number and exit with status
1. `mint script args...` runs a file.

## Tests

- `utils` checks text flags, sorting/listing, recursive matching, tree,
  piped pager output, mount capacities, process columns and `tail -f`.
- `script2`, `lineedit`, and `lineedit_screen` cover the new shell language,
  interactive editor and rendered-console regression; see [Shell](sh.md).
- `script` runs `sh /etc/tests/shell.sh x y` and checks quoting,
  expansion, `export` visibility in a child shell, `$?`, `&&`/`||`,
  `;`, pipelines, a background job with `wait`, redirection and the
  exit status 7.
- `shell2` types an interactive session: an assignment, quoting, a
  background job with its `Done` report, `&&`/`||` and a pipeline.
- `jobcontrol` stops a foreground process group with control Z, checks `jobs`,
  resumes it with `bg`, returns it with `fg`, and terminates it with
  control C.
- `editor` starts `edit` on a new file with two lines queued, waits for
  raw mode, sends cursor up, Home, an insertion, control S and control
  Q, then checks the file contents and that canonical mode was restored.
- `mint` runs `/etc/tests/test.mint` covering recursion, loops, strings,
  comparisons, arguments, file I/O and scoping.
- `kbd` covers control C in the line discipline; `ctrlc` the delivery.
