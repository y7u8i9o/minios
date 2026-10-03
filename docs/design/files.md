# Files

`user/files/` (`/bin/files`) is the file manager, rebuilt on
2026-09-05. `files.c` is the window, `fsops.c` the operations on the
file system.

    files [directory]

## Window

Since 2026-10-03 the window is laid out like GNOME Files around the
folder view of libgui (`folderview.md`), which the file chooser shows as
well. It has a menu bar (File, Edit, View, Go), a tool bar with Back and
Forward followed by the path bar, the location entry, the search field
and the search button of the folder view, a split pane with the places
sidebar on the left and the table of the folder on the right, and a
status bar with the item count, the number of hidden entries and the
selected entry. The program starts in the home folder, or in the folder
named by its argument.

The table, its columns (Name, Size, Type, Modified), the names of the
types, the sorting, the hidden entries (Ctrl+H or the View menu), the
search started by typing, the location entry (Ctrl+L or Go, Location),
the Recent place and the rereading of the folder every two seconds all
belong to the folder view and behave as in the chooser. The View menu
sorts through `folderview_sort` and Edit, Search starts a search.

Enter, a double click or Open enter a folder or open a file with its
handler (`mime_open`), and an opened file is added to the recent list.
Open with... asks for a program, and Open in terminal starts `term -d`
in the selected or current folder. In Recent and in a search the entry
menu offers Open item location, which shows the folder of the entry with
the entry selected. Back and Forward walk a history of 32 folders, which
records every folder the view shows except those reached through Back
and Forward themselves. Alt+Up and Backspace go to the parent folder and
select the folder just left.

The file operations are New folder (Ctrl+N), New file, Rename (F2),
Delete (Delete, after a confirmation naming the entry, with folders
removed together with their contents), Copy (Ctrl+C), Cut (Ctrl+X) and
Paste (Ctrl+V). They act on the path of the selected entry, so they work
on the results of a search and on Recent as well, and new entries go to
the current folder. Copy and Cut remember one path and put it on the clipboard
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

The places Home, Desktop, Programs (`/bin`), Shared files (`/usr/share`),
Fonts (`/etc/fonts`) and Devices (`/dev`) of the former places list are
part of the sidebar of the folder view, which adds Recent, the other
folders of the home folder, the mounted volumes and Computer.

## Operations (`fsops.c`)

`fs_join` and `fs_normalize` build and clean paths, `fs_copy` and
`fs_remove` recurse into directories, `fs_move` renames or copies and
removes, and `fs_tree_size` sums a tree. Results are 0 or `-errno`. Sizes
in the Properties window are formatted by `folderview_format_size`, which
replaced `fs_human_size`, and types are named by `folderview_describe`.

## Toolkit additions

For this program the data views gained an optional `icon` callback in
`struct model`, painted before the first column, a `context` signal on
a right click, `activate` on a double click (400 ms) and
`view_select`, which selects a row and scrolls to it. Four icons were
added to `tools/genicons/genicons.py`: `back`, `forward`, `home` and
`refresh`. The folder view (`folderview.md`) later took over the places,
the listing and the type ahead selection, which became a search.

## Logging and tests

The program prints `files: cd`, `open`, `mkdir`, `create`, `rename`,
`delete`, `copy`, `move` and `terminal in` lines with the paths. The
`gui_files` case opens `/home/desktop`, creates a folder with Ctrl+N,
renames it with F2, deletes it with Delete and Enter, opens
`readme.txt` by typing its name, which searches the folder, and Enter,
and closes the editor and the file manager with Alt+F4.
