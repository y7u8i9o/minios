# Folder view and file chooser

The file manager and the file chooser of libgui share one component, the
folder view (`gui/folderview.h`, `libgui/src/folderview.c`), in the way
GNOME Files and the GTK file chooser share their places sidebar and path
bar. The file chooser (`app_choose_file` in `gui/app.h`,
`libgui/src/filechooser.c`) wraps it in a modal window, and the Files
program (`files.md`) wraps it in its main window. Both were built on
2026-10-03, when the chooser replaced the path prompt that the programs
used for Open and Save.

## The folder view

`folderview_new(bar, split, title, ops, arg)` adds the path bar, the
location entry, the search field and the search button to `bar`, a
horizontal box or tool bar that the program places, and adds the places
sidebar and the table to `split`, a horizontal split pane. The program
keeps the rest of the window, so the chooser adds a name field and its
buttons and Files adds its menus, Back and Forward and a status bar. The
callbacks of `struct folderview_ops` tell the program that a file was
activated (`open`), that the folder, the kind of view or the listing
changed (`changed`) and that the selection changed (`selected`). Two
more let the program decide which files are listed (`filter`, which
folders always pass) and take keys before the folder view does (`key`).
The folder view lives as long as its widgets, because the destroy hook of
the sidebar removes its timer and frees it.

The sidebar lists Recent, the home folder (`$HOME`, else `/home`) and the
usual folders below it that exist (`desktop` or `Desktop`, `Documents`,
`Downloads`, `Music`, `Pictures`, `Videos`), then the system folders that
the file manager had offered before (Programs `/bin`, Shared files
`/usr/share`, Fonts `/etc/fonts`, Devices `/dev`), then every mounted
volume from `/dev/mounts` other than the root, the home volume and devfs,
and last Computer for `/`. Lines separate the four groups, the place of
the current folder is highlighted, and the icons are the single colour
Font Awesome ones of `icons.md`.

The path bar shows one button per folder of the current path, the first
being Home with its icon when the path lies below the home folder and the
root otherwise. The current folder's button is drawn pressed. When the
view goes up, the path bar keeps the folders below, so that their buttons
and Alt+Down lead back, and it forgets them when the view moves to another
branch. Buttons that do not fit give way from the left to one marked
with an ellipsis, which opens the folder above the first button shown.

The table has the columns Name (with the icon of the MIME type), Size,
Type and Modified. In Recent and in a search the second column is the
Location, the folder of the entry with `~` for the home folder. Folders
come first in a folder, the order of Recent is that of the recent list,
and a click on a header sorts. Sizes use decimal units with one decimal
in the separator of the locale (`4,2 kB` in French), and times show the
time of day for today, Yesterday, the day and month for this year and
the full date otherwise, each format being a translatable message.
Names that start with a dot or end with a tilde are hidden until Ctrl+H
shows them. The folder is reread every two seconds when a hash of its
names, sizes, times and kinds changed, and the selection and the scroll
position survive the reread.

Typing a printable character over the table or the sidebar starts a
search below the current folder with that character, and Ctrl+F or the
search button start one as well. The search walks at most 400 folders,
six levels deep, for at most 300 entries whose names contain the text
without regard to ASCII case. Enter in the search field opens the
selected result, or the first one when none is selected, and Escape
returns to the folder. Typing `/` or `~`, or pressing Ctrl+L, shows the
location entry in place of the path bar. Whenever its text grows at the
end, the longest common continuation of the matching names in the folder
of the text is appended and selected, with a slash after a single
folder, so that typing on replaces the completion and Backspace removes
it. Enter shows a folder or opens a file. Alt+Up and Backspace go to the
parent folder, Alt+Home to the home folder, and the folder just left is
selected after going up.

## The recent list

`folderview_recent_add` puts a path at the head of
`$HOME/.local/share/recent-files`, one path per line, and keeps fifty.
The file chooser records every chosen file there and Files records the
files it opens, so that the Recent place of both shows the files used
last that still exist.

## The file chooser

`app_choose_file(app, mode, title, filters, nfilters, path, size)` opens a
760 by 500 window (smaller on a small screen) over the first window of
the program and runs nested `app_step` calls until a file is chosen or
the window is cancelled. `path` gives the initial file or folder and
receives the result. A folder starts the view there, and a file starts
it in the file's folder, with the file selected in open mode and its name
in the name field in save mode. A folder that does not exist falls back
to the nearest existing one above it, and an empty path to the home
folder.

In open mode Enter or a double click on a file chooses it, and Open does
the same for the selected file or for the text of the location entry. In
save mode the name field above the path bar has the focus, with the name
selected up to its extension as GNOME does. Selecting a file copies its
name into the field, Ctrl+L focuses the field, and a `/` or `~` typed
over the table starts a path in it. Enter or Save resolves the name
against the current folder, shows a folder that it names, reports a
folder that does not exist, and asks before an existing file is
replaced. The New folder button creates a folder through a prompt and
enters it. Escape cancels unless it closes the search or the location
entry first, and so does closing the window.

Filters are pairs of a name and patterns separated by spaces. A pattern
with a slash is matched against the MIME type of the name (`image/` with
an asterisk covers every image type of `/etc/mime.types`) and the others
against the name without regard to case. The combo box at the bottom
left selects the filter and is hidden without filters. The context menu
of the table offers Visit file in Recent and in a search, Copy location
and the hidden files switch.

The programs that call the chooser for Open and Save are gedit (with the
filters all files and text files), paint and view (images), player
(audio files), mandel (PNG images), hexview, logview and the export of
the profiler, which no longer asks about replacing a file itself. The Lua binding has `app:open_file(title,
path [, filters])` and `app:save_file(...)` with filters as an array of
`{name, patterns}`, which Code and the Lua synthesizer use.

## Toolkit additions

`textfield_select` places the cursor and the selection of a text field,
and `dialog_message` and `dialog_prompt` (`src/dialog.h`) are
`app_dialog` and `app_prompt` over a given parent window, so that the
chooser's own dialogs stay above it. The tree view and the table no
longer take keys held with Alt, which leaves Alt+Up and the other Alt
accelerators of a window working while a list has the focus. The icons
`folder-new`, `drive`, `recent`, `documents`, `pictures`, `music`,
`videos` and `downloads` were added to `tools/fetch_icons.sh`.

## Tests

`libgui/tests/test_filechooser.c` (part of `make -C libgui check`) builds
a temporary tree with its own MIME table and drives a chooser through
messages to its window. It checks the initial folder and selection, a
MIME filter and the switch to the second filter, Alt+Up and Alt+Down
along the path bar, a search started by typing and ended by Escape, the
location entry with its completion of a folder and of a file, a save
name that names a folder and a new name, the name taken from a selected
file, Ctrl+H, the Recent place and the recent file, Escape and the close
button. The boot case `gui_filechooser` opens `readme.txt` in gedit
through the chooser, saves it under another name with Save as and reads
the recent list. `gui_files` covers the folder view inside Files.
