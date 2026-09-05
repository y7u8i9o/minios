# Reusable line editor

`libedit/` builds `build/libedit/libedit.a` against MiniOS libc. It has no
shell parser dependency. The public API is `libedit/include/edit.h`:

- `edit_open(in, out)` / `edit_close` manage an editor, not its descriptors.
- `edit_readline(editor, prompt, buf, size)` returns the byte length without
  the terminating newline, `-EIO` on EOF, or `-EINTR` on control C.
- `edit_set_completer` installs a callback receiving the line and byte
  cursor. It sets the replacement start and adds owned candidate copies
  with `edit_completion_add`; the editor frees the candidates.
- History has add, load, save, limit, count and indexed get operations.

## Input modes

The prompt is written before reading. If `poll` reports queued canonical
input, the editor reads exactly one line one byte at a time, without
changing terminal modes or echoing it again. This preserves boot tests
that type complete lines before the shell starts and avoids stdio
prefetch consuming the following command.

Otherwise the editor saves termios, clears `ICANON|ECHO|ISIG`, and reads
raw bytes. Disabling `ISIG` makes control C an input byte even though the
shell ignores SIGINT. All raw-mode return paths restore the saved flags.
Bracketed-paste mode is enabled during editing and disabled on return.

## Keys and redraw

| Keys | Action |
|---|---|
| Left/Right, control B/F | Previous/next UTF-8 code point |
| Home/End, control A/E | Start/end of line |
| Backspace, control H; Delete, control D | Erase before/at cursor; D on empty line ends input |
| control K/U/W | Erase suffix/prefix/previous word |
| Alt B/F | Previous/next word |
| Up/Down, control P/N | History, preserving the unfinished draft |
| control R | Reverse substring search; repeated R finds earlier entries |
| Tab | Complete the current word |
| control L | Redraw |
| control C | Cancel line |

`keys.c` decodes CSI/SS3 keys with a bounded escape timeout. The redraw in
`edit.c` uses carriage return, erase-to-end and relative cursor movement;
it does not request a cursor-position report. `TIOCGWINSZ` is read on each
redraw. UTF-8 is decoded with `mbrtowc`, widths come from `wcwidth`, and
prompt CSI sequences consume no cells. Previous row/cursor measurements
identify the region to clear. An explicit wrap handles the difference
between the console's immediate wrap and the window's delayed wrap.

## History and completion

History is bounded, skips empty lines and consecutive duplicates, and
uses one line per file record. The shell controls the path and persistence.
The default completer walks the current directory, path prefixes, and
executable files in PATH at command position. Directories end in `/`.
Tab inserts the common prefix, adding a space for a unique non-directory
match. Callers can replace this policy without changing input handling.

Current completion does not list ambiguous candidates or shell-quote
filenames containing whitespace. Width calculations understand Unicode,
but rendering is limited by the console/window's existing glyph and cell
support. These are not promises of full readline or terminal compatibility.

## Tests

`lineedit` waits for raw mode before sending extended scancodes, fixes
`cho hello` using Home, completes `hexd` to `hexdump`, recalls history and
exits with the expected status. `lineedit_screen` checks framebuffer cells
after repeated invalid commands to detect stale and duplicated text.
`shell`, `shell2`, `jobcontrol`, `ctrlc`, `editor`, `gui_term` and
`comp_shell` cover integration with canonical input and terminal ownership.
