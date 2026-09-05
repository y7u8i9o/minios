# Files

`user/files/` (`/bin/files`) is the file manager, rebuilt on
2026-09-05. `files.c` is the window, `fsops.c` the operations on the
file system.

    files [directory]

## Window

The window has a menu bar (File, Edit, View, Go), a tool bar (Back,
Forward, Parent folder, Home, the path field, Refresh, a Hidden check
box), a split pane with the places list on the left and the directory
table on the right, and a status bar with the item count and the
selected entry.

The table is a `table` widget over a `struct model` with the columns
Name (with the icon of the entry's MIME type, `mime_icon`), Size
(`fs_human_size`), Type and Modified (`strftime`). Directories come
first; clicking a header sorts by that column, the View menu sorts as
well. Types are named from the MIME type (`Folder`, `Text`, `C source`,
`Shell script`, `PNG image`, `WAV audio`, `Launcher`), executables
without an extension are `Program`. Hidden entries (names starting with
a dot) are listed when the check box is on.

Enter, a double click or Open enter a directory or open a file with its
handler (`mime_open`); Open with... asks for a program; Open in
terminal starts `term -d` in the selected or current directory.
Back and Forward walk a history of 32 directories; going up selects
the directory just left. Typing letters while the table has focus
selects the first entry whose name starts with the typed text (the
buffer resets after a second); Backspace goes to the parent.

File operations: New folder (Ctrl+N), New file, Rename (F2), Delete
(Delete, after a confirmation naming the entry; directories are
removed with their contents), Copy (Ctrl+C), Cut (Ctrl+X) and Paste
(Ctrl+V). Copy and Cut remember one path and put it on the clipboard
as text; Paste copies or moves it into the current directory, refusing
an existing name and naming a copy into its own directory `Copy of`.
Moves try `rename` and fall back to copy and remove across file
systems. Properties (Alt+Enter) opens a modal window with the name,
location, type, size (directories: the number of files and folders and
the total below them), inode and modification time. Errors are shown
in a dialog with the `strerror` text.

The right button on the table selects the row under the pointer and
opens a context menu (the table's `context` signal): the entry menu
with Open, Open with, Open in terminal, Copy, Cut, Paste, Rename,
Delete and Properties, or the directory menu with New folder, New
file, Open in terminal, Paste, Refresh and Properties.

A two second timer rereads the directory and refreshes the table when
the names, sizes, times or kinds differ, so files written by other
programs appear; the selection is kept by name.

The places list has Home (`/home`), Desktop (`/home/desktop`), Root,
Programs (`/bin`), Shared files (`/usr/share`), Fonts (`/etc/fonts`)
and Devices (`/dev`).

## Operations (`fsops.c`)

`fs_join` and `fs_normalize` build and clean paths, `fs_copy` and
`fs_remove` recurse into directories, `fs_move` renames or copies and
removes, `fs_tree_size` sums a tree. Results are 0 or `-errno`.

## Toolkit additions

For this program the data views gained an optional `icon` callback in
`struct model`, painted before the first column, a `context` signal on
a right click, `activate` on a double click (400 ms) and
`view_select`, which selects a row and scrolls to it. Four icons were
added to `tools/genicons/genicons.py`: `back`, `forward`, `home` and
`refresh`.

## Logging and tests

The program prints `files: cd`, `open`, `mkdir`, `create`, `rename`,
`delete`, `copy`, `move` and `terminal in` lines with the paths. The
`gui_files` case opens `/home/desktop`, creates a folder with Ctrl+N,
renames it with F2, deletes it with Delete and Enter, opens
`readme.txt` by typing its name and Enter, and closes the editor and
the file manager with Alt+F4.
