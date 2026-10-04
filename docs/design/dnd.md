# Drag and drop

Drag and drop moves data between windows of any programs through the
data device of the X12 protocol, the interface that also carries the
clipboard. A drag carries one or more MIME types and an action, copy or
move. Built on 2026-10-04.

## Protocol (`protocol/data.xml`, version 2)

Version 1 (M25) already had `data_device.start_drag`, the events
`enter`, `motion`, `leave` and `drop`, `data_offer.accept`, `receive` and
`finish`, and `data_source.dnd_drop_performed` and `dnd_finished`.
Version 2 appends the actions without changing an existing opcode:

- `data_source.set_actions(actions)`: what the source allows, a mask of
  1 (copy) and 2 (move). A source that never calls it allows copy.
- `data_offer.set_actions(actions, preferred)`: what the target supports
  and the action it prefers. An offer that never calls it supports copy
  and move and prefers copy.
- `data_offer.source_actions(actions)`: sent with a new drag offer.
- `data_offer.action(action)` and `data_source.action(action)`: the action
  the compositor chose, sent when it changes; 0 means none.

The data travels as in the clipboard: the receiver passes the write end
of a pipe with `receive`, and the compositor forwards it to the source
with `send`. A receiver may call `receive` before the drop to read the
data during the drag, which the folder view uses to choose move or copy.

## Compositor (`user/compositor/data.c`)

`start_drag` is accepted with the serial of the button press in the
origin surface (`seat_validate_drag`). The pointer leaves the origin
(`seat_drag_started`), and while the drag runs, pointer events go to the
data device only. The icon surface takes the role `dnd-icon`; its attach
offsets move it relative to the cursor (`data_icon_committed`), and the
scene draws it above every other surface and leaves it out of hit
testing. Each surface the cursor enters receives a new offer with the
types and the source actions, then `enter` with an enter serial that
`accept` must quote, and `motion` while the cursor moves over it.

The action is chosen from the actions both sides allow: Ctrl forces copy
and Shift forces move when allowed, otherwise the target's preference,
else copy, else move (`drag_choose_action`). It is chosen again when a
side changes its actions, when the cursor enters another surface and when
a modifier key changes, and the log reports `drag action copy|move|none`.

The release of the button ends the drag (`drag_finish`). A target that
accepted a type and has an action receives `drop`, and the source
`dnd_drop_performed`; `finish` of the offer sends `dnd_finished` to the
source. Otherwise the target receives `leave`, the source `cancelled`,
and the log reports `drag cancelled`. Escape cancels the drag at once,
and neither of its key events reaches a client. The release of the
button that started a cancelled drag is not delivered either
(`seat_drag_ended`). A source that goes away during the drag cancels it,
and so does a client that disconnects. After the drag the pointer enters
the surface under it.

## Client library (`lib/libgui/src/client.c`, `gui/client.h`)

`gui_drag_start(window, items, n, actions, icon, hot_x, hot_y)` copies up
to eight items (a MIME type and its data each), creates the source and
the icon surface (an ARGB image in device pixels at the window's scale,
`hot` being the point under the cursor), and starts the drag with the
serial of the last button press. The window receives no release of the
button; `WM_DRAG_END` with the action performed, or 0, reports the end.
A release that does arrive means the compositor refused the drag, which
then ends with 0 as well.

A drag over a window of the process queues `WM_DRAG_ENTER` and
`WM_DRAG_MOTION` with the contents coordinates, the chosen action and the
source actions; a change of the action alone repeats `WM_DRAG_MOTION`.
The window answers with `gui_drag_accept(mime, actions, preferred)`, NULL
to refuse, and `gui_drag_offers(mime)` tells the offered types.
`gui_drag_peek(mime, &len)` reads the data before the drop: the first call
starts the read and returns NULL, `WM_DRAG_MOTION` follows when the data
has arrived, and later calls return it. On a drop the library reads the
data through a non-blocking pipe polled by `gui_next_event` (and by
`app_step` through `gui_transfer_fd`), then finishes the offer and queues
`WM_DROP`; `gui_drop_data` returns the data with a terminating NUL byte,
at most `GUI_DND_MAX` (16 MiB). Data read before the drop is used for the
drop without a second read. A drop of a drag this process started copies
the items directly, because the compositor would route the transfer back
to this process.

## Framework (`lib/libgui/src/window.c`, `gui/widget.h`)

Widgets receive `EV_DRAG_MOVE`, `EV_DRAG_LEAVE`, `EV_DROP` and
`EV_DRAG_END` with a `struct drag_event`. The window delivers
`EV_DRAG_MOVE` to the widget under the cursor and its ancestors until one
returns 1; that widget becomes the drop target, its answer
(`accept_mime`, `accept_actions`, `preferred`) goes to `gui_drag_accept`,
and a previous target receives `EV_DRAG_LEAVE`. `EV_DROP` goes to the drop
target, followed by `EV_DRAG_LEAVE`. Drops on the popup surface of a
window are refused.

A widget starts a drag when the pointer has moved more than
`DRAG_THRESHOLD` (4) logical pixels from the press (`widget_drag_moved`),
with `widget_drag_start(w, items, n, actions, icon, label)`, which draws
the drag image: a rounded tile in the field colour with the icon (at most
20 pixels) and the label, at 88 percent opacity, 12 pixels below and to
the right of the cursor tip. The window forgets the mouse capture, since
no release follows, and the widget receives `EV_DRAG_END`.

- Tables and tree views (`widgets/models.c`) emit `drag_begin` for a row
  pressed and moved, `drag_end`, and for drags over them `drag_motion`,
  `drop` and `drag_leave`, all with `struct sig_drag` (row, position,
  event). The accepted row is outlined in the accent colour, or the whole
  view when the handler sets the row to -1.
- The canvas emits the same four signals with the row -1.
- The editor (`widgets/editor.c`) drags its selection as `text/plain`: a
  press inside the selection waits for a move, and a release without one
  places the cursor. Copy is always allowed and move unless the editor is
  read only; a move to another place removes the text if it is still
  there. Text dragged over the editor shows an accent caret at the drop
  position and is inserted there as one undo step; the editor's own
  selection moves there, as one undo step, and a drop into the dragged
  text is refused. Copy is preferred for text of other programs and move
  for the editor's own. The owner may take a drag first through the
  signals `drag_motion` and `drop`.
- The text field drags its selection in the same way, except when it is
  masked; it takes no drops.

## Files and their formats (`lib/libgui/src/fileops.c`, `gui/fileops.h`)

Files are dragged as `text/uri-list` (one `file://` URI per line ending
in CR LF, every byte outside the unreserved characters and the slash
percent encoded) and as `text/plain` with the path. Recursive copy,
remove and move moved here from Files (`fileops_copy`, `fileops_remove`,
`fileops_move`), so that Files, the file chooser and the desktop share
them. `fileops_drop_action` gives the preferred action of a drag of a
uri-list into a folder: move when every file is on the file system of the
folder, as in GNOME Files, copy otherwise or while the list is unknown,
and 0 for a folder dropped into itself or files dropped into the folder
they are in. `fileops_drop` copies or moves the files: a copy into its own
folder is named "Copy of NAME", an existing name is never replaced
(`EEXIST`), and a folder is never copied into itself (`EINVAL`).

## Programs

- The folder view (Files and the file chooser, `folderview.md`) drags a
  row as `text/uri-list` and `text/plain` with the icon and name of the
  file. Files dropped on a folder row, on the empty part of the table
  (the folder shown; not in Recent or a search) or on a place of the
  sidebar other than Recent are copied or moved there, an error is shown
  in a dialog, and the listing is reread. Files logs
  `files: drop moved|copied N into DIR` through `folderview_on_dropped`.
- The desktop (`desktop.md`) drags its icons the same way, and takes
  files dropped on a folder icon or on the desktop, which go into the
  folder or into `~/desktop` (`desktop: drag PATH`,
  `desktop: drop moved|copied N into DIR`).
- gedit opens the first file of a `text/uri-list` dropped on the editor
  (`gedit: opened PATH from a drop`); text dropped on it is inserted by
  the editor widget.
- The terminal types the paths of dropped files into the program, each
  in single quotes followed by a space, as a bracketed paste when the
  program asked for it (`term: dropped N paths`).

## Tests

- `make check` for libgui: `lib/libgui/tests/test_dnd.c` covers the uri-list
  format, `fileops_drop_action` and `fileops_drop` over a temporary tree,
  a folder view row dragged out and a file dropped on a folder row, the
  editor's drag of its selection, a move inside the editor with its single
  undo step, text of another program inserted at the drop caret, a move to
  another program, a click inside the selection, and a text field's drag
  with a masked field refusing it. The fake client records the drags and
  supplies the offers and the data.
- `comp_data`: besides the version 1 drag, a source allowing copy and move
  over a target preferring move (`comptest drag-source`, `drag-target`):
  the action is move, Ctrl makes it copy, Escape cancels the drag with
  `leave` to the target and `cancelled` to the source.
- `gui_dnd`: `/bin/dndtest` drags three files from its source window: into
  its target window, which reads the paths before the drop and moves the
  file into its folder, into gedit, which opens it, and into the
  terminal, whose shell receives `cp `, the quoted path and the rest of a
  command line that the test checks by its result.
