# Terminal

`user/term/` (`/bin/term`) is the terminal window. It was rebuilt on
2026-09-05 on the emulator in `vt.c`; the window code in `term.c` runs
the programs, paints and handles keys, `select.c` holds the mouse
selection and the clipboard, and `term.h` the tab structure they
share.

    term [-d directory] [command [argument...]]

Without a command the shell runs; `-d` sets the working directory of
the program (the Files program opens terminals this way).

Each child starts with `PATH=/bin`, `HOME=/home`, `USER=user`,
`SHELL=/bin/sh` and `TERM=xterm-256color`. The interactive shell loads
the same profile, `.shrc` and history as the framebuffer-console shell;
the latter starts with `TERM=minios`. See [Shell](sh.md) and
[libedit](libedit.md). Terminal descriptors are read/write, so a pager
whose stdin is a pipe can read keys through a duplicate of stdout without
opening the unrelated framebuffer console.

## Emulator (`vt.c`)

`struct vt` has a grid of `struct vcell` (code point, combining mark,
foreground, background, attributes), a second grid for the alternate screen, the
cursor, the scrolling region, the current rendition, a saved cursor,
the modes and the parser state. Colours are `0x00rrggbb`; the bit
`VC_DEFAULT` marks the default foreground or background so the window
can paint them with its own colours and the selection can invert them.
Lines scrolled off the top of the main screen go to a ring of
`SCROLLBACK` (2000) lines, trimmed of trailing blanks. The alternate
screen has no scrollback. Rows are numbered for the window from the
oldest scrollback line (0) through the screen rows, so a view offset
is a plain subtraction.

`vt_resize` keeps the top left of the screen. When rows are removed,
lines above the cursor go to the scrollback first; when rows are added,
lines come back from the scrollback and the cursor moves down with
them. Columns are not rewrapped.

The parser handles UTF-8 (invalid bytes become U+FFFD), the C0 controls
BEL (sets `bell`), BS, HT (stops every 8 columns), LF, VT, FF, CR, and
these sequences:

- ESC `7` `8` (save, restore cursor), `D` (index), `E` (next line),
  `M` (reverse index), `c` (reset); charset designations and DCS
  strings are consumed.
- CSI `@` `A` `B` `C` `D` `E` `F` `G` `H` `J` (0 to 3, 3 clears the
  scrollback) `K` `L` `M` `P` `S` `T` `X` `Z` `d` `f` `` ` `` `r`
  (scrolling region) `s` `u`; `n` answers `5n` and `6n`, `c` answers
  with a VT220 identification.
- SGR `m`: 0, 1 (bold: the bright variant of a palette colour), 2
  (faint), 3, 4 (underline), 7 (reverse), 9 (strike), 22 to 29, 30 to
  37, 39, 40 to 47, 49, 90 to 97, 100 to 107, `38;5;n` and `48;5;n`
  (256 colours), `38;2;r;g;b` and `48;2;r;g;b`.
- Modes `4` (insert), `20` (LNM: LF also returns to column 0; on by
  default because the tty line discipline writes a bare LF) and the
  private modes `?1` (application cursor
  keys), `?6` (origin), `?7` (autowrap, with a pending wrap at the last
  column as in xterm), `?25` (cursor), `?47`, `?1047`, `?1049`
  (alternate screen, 1049 saves the cursor), `?2004` (bracketed paste).
- OSC `0` and `2` set the title, terminated by BEL or ESC `\`.

The palette is the one of Visual Studio Code's dark theme; the default
foreground is `0xd4d4d4` on `0x1e1e1e`.

## Window (`term.c`)

The window holds a `tabs` widget whose title row is hidden while there
is one page (`tabs_set_autohide`); each page is a canvas with one
`struct tab`: the emulator, the pseudo terminal master, the child's
pid, the view offset and the selection. The text is DejaVu Sans Mono
from `/etc/fonts` at `term_font_px` pixels (from `/etc/desktop.conf`,
default 13, set in the Appearance page of Settings), rasterised with
antialiasing at the output scale; the cell is the advance of `M` by
the font height (8 by 17 at 13 px). The grid is the canvas size less 4
pixels of padding on the left and right, 3 above and below, and the
scrollback bar on the right; every tab has the same grid and receives
`TIOCSWINSZ`, which the kernel turns into SIGWINCH.

Wide characters occupy two cells and combining marks attach to the
previous cell, as `unicode.md` describes.

Painting fills runs of one background, draws each non blank cell with
`painter_text_font` at its column (the advance is not an integer number
of pixels, so runs are not drawn as strings), then the underline and
strike lines, the cursor (a filled block that blinks at 530 ms while
the window has focus, an outline when it does not) and the bar. Output
is collected from the master by the application loop and painted at
most once per 16 ms. The master ignores `O_NONBLOCK`, so the reader
polls before each further read.

Keys: printable characters and control characters go through as typed
(Alt prefixes ESC), cursor and function keys send the xterm sequences
with the modifier parameter (`CSI 1;m X`, `CSI n;m ~`), and application
cursor keys use `SS3`. Shift+PageUp, Shift+PageDown, Shift+Home and
Shift+End scroll the view, the wheel scrolls by three lines, and any
key or new output returns to the live screen. Ctrl+Shift+C copies the
selection to the clipboard, Ctrl+Shift+V, Shift+Insert and the middle
button paste it (wrapped in the bracketed paste markers when the mode is
on), Ctrl+Shift+T opens a tab running the same command, Ctrl+Shift+W
closes it, Ctrl+PageUp and Ctrl+PageDown switch tabs, Ctrl+plus,
Ctrl+minus and Ctrl+0 change the font size for the window. The right
button opens a context menu with the same actions and Clear scrollback.

The selection is made with the left button: a drag selects characters,
a double click selects a word, a triple click a line. It is kept in
line numbers of the emulator so it stays on scrolled lines. A click in
the bar moves the view so that the clicked point is the centre of the
page, and dragging there follows the pointer.

A tab closes when its program exits or on Ctrl+Shift+W (the process
group gets SIGHUP then SIGKILL); the window exits with its last tab.
The window title is the title set with OSC, followed by " - Terminal".

## Logging and tests

The window prints `term: shell pid P on /dev/ptsN, CxR cells of WxH`,
`term: size CxR` on grid changes, `term: view N` on scrolling and
`term: shell exited, status S`. The `gui_term` case types commands
through the keyboard driver, checks the file the shell writes, the
background and the antialiased prompt pixels, shrinks the window by
240 by 96 pixels and checks `stty size` (19 rows, 50 columns), then
scrolls back with Shift+PageUp; `gui_term_scale2` runs it on a
2048x1536 output at scale 2.
