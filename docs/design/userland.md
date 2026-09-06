# User space applications

M16 completes the user environment: the shell, the coreutils, a screen
editor and a scripting interpreter, together with the kernel support
they need (raw keyboard mode, console cursor control, a sleep call).

## Kernel support

- Terminal modes. The console line discipline in `drivers/ps2kbd.c` has
  the flags `ICANON`, `ECHO` and `ISIG` (`minios/abi.h`), read and set
  through `ioctl` on `/dev/console` with `TCGETS` and `TCSETS`;
  `TIOCGWINSZ` reports the text size. Without `ICANON` every byte is
  delivered as soon as it is typed and the cursor keys (arrows, Home,
  End, PageUp, PageDown, Delete) arrive as VT100 escape sequences; the
  Escape key itself sends `ESC`. Leaving canonical mode releases a
  partially typed line. Control C sends `SIGINT` and control Z sends
  `SIGTSTP` while `ISIG` is set. libc provides `tcgetattr` and `tcsetattr` (`termios.h`) and
  `ioctl` (`sys/ioctl.h`).
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
in [Shell](sh.md). Interactive history, completion and key handling live in
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

`mandel` renders progressively. The complex plane is held in 64 bit
fixed point with 26 fraction bits, the view is a centre and a scale in
units per pixel, and the scale halves per zoom level (levels -4 to 16,
the iteration limit grows by 24 per level from 64). The renderer keeps
one iteration count per pixel (-1 while unknown) next to the colour
image and runs passes with sample spacing 32, 16, 8, 4, 2 and 1. The
first pass samples the grid and fills 32 pixel blocks. Each later pass
refines every block of the previous pass: it samples the midpoints of
the four edges, and when all eight boundary samples agree the block is
marked solid (every pixel takes that count, so no later pass samples
inside it), otherwise the centre is sampled and the block is split by
the next pass. Samples are stored on the pixel grid, so points shared
by neighbouring blocks are computed once. Work is driven by a repeating
application timer in slices of 30 ms followed by one invalidation of the
canvas, so input is handled between slices. Panning shifts the existing
image and rerenders, dragging shows the shifted image until the button
is released. Serial lines `mandel: rendering WxH at zoom level N` and
`mandel: render complete in T ms` mark each render.

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
