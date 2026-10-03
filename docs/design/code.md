# Code, the source editor

`/usr/share/apps/code.lua` is a source editor for C, Lua and shell
scripts, written in Lua on the `gui`, `sys` and `fs` modules. `/usr/bin/code`
starts it with the arguments; `Code.app` on the desktop, the launcher
menu and the file types `text/x-lua`, `text/x-csrc` and
`text/x-shellscript` point at it.

## Window

The window has a menu bar (File, Edit, Run, View), a tool bar (New, Open,
Save, Find, Run, Stop), a split pane with the editor above the output
pane on the left and the outline table on the right, and a status bar
with the file and its language, the cursor position and the state of a
run. The editor and the output pane use DejaVu Sans Mono at 13 pixels;
`settings.font` and `settings.font_px` in `$HOME/.config/code.lua`
select another font file or size. The editor has line numbers;
Ctrl+N, Ctrl+O, Ctrl+S, Ctrl+Q, Ctrl+Z, Ctrl+Y, Ctrl+F and Ctrl+G are
the accelerators of the menu items, F5 runs and F6 stops. The output pane
is a read-only editor. The outline lists the definitions the language
pattern finds in the text with their line numbers; activating a row
moves the cursor there. Closing a window with unsaved changes asks
whether to save.

Run saves the file, starts the command of its language through
`sys.spawn_pipe` with standard output and error joined, appends what
arrives to the output pane from an `app:watch` on the pipe, and appends
the exit status or the signal when the pipe ends. Stop sends `SIGTERM`.
The program logs what it did on standard output (`code: opened`,
`code: run:`, `code: output:`, `code: run exit`), which the boot test
reads.

## Languages

`languages` at the top of the file is a table of entries with the name,
the file extensions, the highlighter (a name for `editor:highlight` or a
table with keywords, comment and string syntax), the command that runs a
file (`$FILE` stands for the path), one or two patterns that find
definitions for the outline, and the text of a new file:

    lua = {
      name = "Lua", extensions = { "lua" }, highlight = "lua",
      run = { "lua", "$FILE" },
      definition = "^%s*local%s+function%s+([%w_.:]+)",
      template = "print(\"hello\")\n",
    }

A language is added by adding an entry. `$HOME/.config/code.lua` is
loaded, when it exists, with `languages` in its environment; a user
adds or changes entries there without editing the installed program. A
highlighter table for a new language needs no C code, since
`highlight_lang` reads the keywords and the comment and string syntax
from the table.

## Toolkit and bindings

The port added to the toolkit `highlight_lang`, a highlighter driven by
`struct highlight_language` (keywords, line comment, block comment
delimiters, quotes, preprocessor character), with the descriptions
`highlight_language_c`, `highlight_language_lua` and
`highlight_language_sh`; `highlight_c` and `highlight_sh` remain.

The `gui` module gained the constructors `editor`, `menubar`, `menu`,
`popupmenu`, `toolbar`, `statusbar`, `treeview` and `table`, and these
methods: on menus `menuitem(text [, icon])`, `separator()` and
`popup(x, y)`; on tool bars `tool(icon, tip)`; on status bars
`field(stretch)`; on every widget `icon(name)`; on editors `text`,
`lines`, `line(i)`, `wrap`, `numbers`, `readonly`, `highlight`, `font(path [, px])`, `go(line [, col])`,
`cursor`, `undo`, `redo`, `search(needle [, forward])` and `modified`;
on tree views and tables `model(t)`, `refresh`, `rows([index])`,
`selectrow(row)`, `expand(row [, on])` and `column(col [, width])`. A
model table has `rows(parent)`, `child(parent, index)`, `columns`,
`cell(row, col)`, `header(col)` and `sort(col, descending)`; the root is
`-1`, rows are the numbers the model chooses, columns and indexes start
at 1. The `selected` and `activate` signals of the two views carry
`row`; `context` carries `x` and `y`. The `sys` module gained
`spawn_pipe(program, args...)`, which returns the pid and the read end
of a pipe joined to the child's output, `read(fd [, max])`, `close(fd)`
and `wait(pid [, nohang])`.

## Tests

`user/lua/tests/gui.lua`, run on the host by `make check-lua`, covers the
editor methods and the highlighter table, a table with a model, its
refresh and column widths, and the menu, tool bar and status bar
constructors. `tests/cases/gui_code` starts the editor on the compositor
with `/etc/tests/sample.lua`, presses F5 and then Ctrl+Q, and expects
the log lines of the open, the run, the output and the exit status.
