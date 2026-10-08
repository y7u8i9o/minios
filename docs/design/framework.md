# Application framework

M21 adds a retained widget framework to `lib/libgui/` beside the M19
toolkit (`gui/widgets.h`), which remains until the applications move to
the framework in M22. The framework consists of an application object
(`gui/app.h`), a widget object model with signals (`gui/widget.h`), a
painter with a clip stack (`gui/paint.h`), a theme (`gui/theme.h`),
layout containers and the core widgets.

## Application object (`src/app.c`)

`app_create` connects to the window server, initialises the default
theme and loads its font. `app_window` creates a top level window widget
backed by a `gui_window`. `app_run` repeats `app_step`: paint every
window, compute the next timer deadline, `poll` the server queue and
the watched descriptors (`app_watch_fd`), dispatch server messages to
windows by id, run due timers (`app_timer_add`, repeating or one shot),
destroy windows that were closed, and paint again. The loop ends when
`app_quit` is called or the last window is closed. Painting once per
iteration means a burst of input costs one repaint. The loop skips a
window while its last frame is pending (`gui_frame_pending`). The dirty
flags collect the changes, and the window paints after the frame
callback.

`app_create_detached` (used by the host tests) builds an application
over the fake client in `tests/fake_client.c` with the builtin font.

## Widget model (`src/widget.c`, `src/window.c`)

- `struct widget` contains the class pointer, tree links, geometry inside
  the parent, flags (visible, enabled, focusable, focused, hover,
  pressed, dirty, child_dirty, needs_layout), the application's size
  hint and the measured hint, stretch factors, alignment, margin, grid
  placement, an id, a text, integer state (`value`, `min`, `max`), the
  signal handlers, an accelerator and a user pointer. Classes extend the
  structure by declaring a larger `size`.
- `struct widget_class` supplies `measure`, `layout`, `paint`, `event`
  and `destroy`. Events (`struct event`) are mouse down, up, move and
  wheel in local coordinates, key down and up, focus in and out, enter
  and leave. `widget_dispatch` offers an event to a widget and bubbles
  it to the ancestors, translating coordinates on the way.
- Signals: `widget_connect(w, "clicked", fn, arg)` appends a handler;
  `widget_emit` runs the handlers of that name in order until one
  returns non zero. `widget_destroy` emits "destroy" (no arguments) after
  the children are gone and before the class destructor, so an owner of
  a reference (the Lua binding) can drop it. Each signal name has a fixed argument structure
  (`sig_click`, `sig_change`, `sig_key`, `sig_select`, `sig_resize`,
  `sig_scroll`, `sig_paint`).
- Windows translate server messages: mouse messages go to the widget
  under the cursor or to the capturing widget (`widget_capture`, active
  until the button is released), with enter and leave events for
  hover; a press focuses focusable widgets. Keys go to the focused
  widget, then Tab and Shift+Tab traverse focusable widgets in tree
  order, then accelerators (`widget_set_accel`) and mnemonics (an `&`
  in a caption with Alt) emit `clicked`, and finally the window emits
  `key`. `WM_CLOSE` emits `close`; the window closes unless a handler
  consumes it. `WM_RESIZED` relayouts and emits `resize`.

## Layout (`src/layout.c`)

Measurement runs bottom up: `widget_measure` calls the class `measure`
and overrides it with the application's hint (`widget_set_hint`,
`widget_set_min`, `widget_set_max`), confining preferred sizes inside the
bounds. Layout runs top down after measurement.

- Padding: a container's inner padding is `widget_set_padding`, else
  the theme padding for a top level window and none for nested
  containers (tool bars and status bars use 2 px), so nesting does not
  multiply margins; full bleed applications set the window padding to 0.
- `box`: children along one axis with the padding and theme spacing; extra
  space goes to children in proportion to their stretch factor, a
  shortfall shrinks children in proportion to their preferred size but
  not below their minimum; the cross axis fills or aligns according to
  `align_x` and `align_y`; margins apply around each child.
- `grid`: children placed with `widget_set_grid(row, col, rowspan,
  colspan)`; column widths and row heights come from single span
  children, spanning children enlarge their last row or column when
  the span is too small; `grid_set_stretch` or a child's stretch
  factor distributes extra space.
- Windows lay out like a vertical box.

## Theme and painter (`src/theme.c`, `src/paint.c`)

`struct theme` contains named colours (`TC_*`) and metrics (`TM_*`) at
scale 100, a scale in percent, and the font. `theme_apply` loads the
outline font at `TM_FONT_PX` scaled (DejaVu Sans at 14 pixels by
default, the builtin 8x16 font when the file is missing);
`app_theme_changed` reapplies it and relayouts every window.

`struct painter` wraps a surface with an origin and a clip rectangle
stack: `painter_push(x, y, w, h)` enters a child area, so widgets paint
in local coordinates and every primitive (`fill`, `frame`, `line`,
`rounded`, `text`, `blit`, `mask`, `focus_ring`) is clipped to the
widget. Text uses the theme font. Widgets measure text at the scale of
their window with `widget_text_width` and `widget_text_index`
(`widgets.md`).

## Drag and drop

Widgets receive `EV_DRAG_MOVE`, `EV_DRAG_LEAVE`, `EV_DROP` and
`EV_DRAG_END` with a `struct drag_event`; the first widget from the one
under the cursor up through its ancestors that returns 1 for
`EV_DRAG_MOVE` is the drop target, and its answer goes to the compositor.
`widget_drag_start` starts a drag with a drawn image of an icon and a
label. The mechanism, the widgets that use it and the programs are
described in `dnd.md`.

## Partial redraw

`widget_invalidate` marks a widget dirty and its ancestors child dirty.
`window_paint` lays out what changed (Layout of changed widgets, below)
and calls `gui_begin_paint`, which moves the window to a free
buffer (`gui.md`). It then walks the tree: a dirty widget and everything below it are
repainted, a widget with only dirty descendants recurses without
painting. Each repainted subtree gives one damage rectangle, and close
rectangles merge in a `rect_set`. `window_paint` returns their union,
which `app_set_damage_log` prints for the tests.

## Partial repaints and scrolling by copy (K3 of `docs/plan/widgets.md`)

`widget_invalidate_rect(w, r)` marks a part of a widget. A widget records
up to three separate dirty rectangles. Overlapping rectangles join, and
a fourth rectangle joins the third. The paint repaints each rectangle
separately: the paint function of the widget with the clip of the
rectangle, then the children inside it. Labels, buttons, check boxes,
radio buttons and separators have the bit `transparent`. They paint no
background, and their invalidation becomes a rectangle of the nearest
opaque ancestor.

`widget_scroll_area(w, r, dy)` records a move of the pixels of `r` by
`dy`. The next paint moves them with `gfx_move_rect` in the window
surface and repaints the exposed rows. The method relies on
`gui_buffers_switch`, which gives the current buffer the latest
contents. The moved area is damage for X12. The function repaints `r`
instead when a popup lies in the window surface, when the widget waits
for another partial paint, or when the move is not smaller than `r`. The
paint repaints `r` when the widget is repainted whole or when `r` is not
wholly visible. The list view, the tree view and table, the editor and
the scroll area scroll this way. Their paint functions draw only the
rows inside the clip. The scroll area places its content in a viewport
inside its frame, so the content never covers the frame.

## Layout of changed widgets (K2 of `docs/plan/widgets.md`)

Measurements are cached. `widget_relayout(w)` sets `needs_measure` on `w`
and its ancestors and `needs_layout` on `w`, and it invalidates `w`.
`widget_measure` computes only marked widgets. A measurement that
changes sets `needs_layout` on the parent. The pass therefore ends at the
first ancestor whose measurement does not change. The layout pass of
`window_paint` lays out the marked widgets and the children whose
rectangle changed. Layout functions place children with
`widget_set_rect` and hide them with `widget_show_in_layout`. Both mark a
changed child as `moved`. A container whose children moved is repainted.
A resize, a new scale or a new theme calls `window_relayout_all`, which
measures and lays out every widget. Floating popups do not lay out the
window. Painting skips subtrees outside the clip and clears their marks.
A scroll area moves its content without a layout.

## Core widgets (`src/widgets/`)

label, button (`clicked`), check box and radio button (`toggled`, radios
exclusive among siblings), separator, canvas (`paint`, `press`,
`release`, `motion`, `wheel`, `key`; not focusable unless the program
sets `focusable`), text field (`changed`,
`activate`; cursor, selection with Shift and the mouse, Ctrl+A, C, X,
V through the server clipboard, and a masked mode for passwords that
shows one `*` per byte and never copies, `textfield_set_masked`), list
view (`selected`, `activate` also on a double click; keyboard
navigation, wheel, internal scroll track), scroll bar
(`scrolled`; thumb dragging, paging, wheel, keys) and scroll area (a
viewport over its content box, `w->user`, with bars shown when the
content overflows).

## Tests

- `make check` compiles libgui (without `client.c`) and libfont with
  the host compiler against `tests/fake_client.c` and runs
  `tests/test_framework.c`: box and grid layout, signal order and
  consumption, partial redraw rectangles, focus traversal, mnemonics,
  clicks, text editing with cut and paste, list navigation.
  `tests/test_scale.c` runs the widgets at scale 2 (`widgets.md`). The
  tests share their event messages through `tests/events.c`.
- `gui_app`: `/bin/apptest` with two windows, a repeating timer that
  stops after three ticks and writes to a pipe, a watched pipe, a
  button, a text field and a list; the kernel injects a click, Tab,
  keys, a list click and Alt+F4, and the expected log includes a damage
  rectangle of the button alone.
- Pipes gained `poll` support (`kernel/ipc/pipe.c`) because the
  framework watches descriptors with `poll`; descriptors without a
  `poll` operation are reported as always ready by the kernel.

## M22: images, controls, data views, the editor

### Images (`src/inflate.c`, `src/png.c`, `gui/image.h`)

Since C1 of `docs/plan/codecs.md` the inflate code and the PNG codec are
in libcodec (`lib/libcodec/src/inflate.c`, the module `png.so`), and the
functions below call it (`codecs.md`).

`zlib_inflate` decodes RFC 1950 streams: stored, fixed and dynamic
Huffman blocks through canonical code tables, with the Adler-32 trailer
checked. `image_decode` parses PNG chunks (IHDR, PLTE, tRNS, IDAT,
IEND), inflates the image data, reverses the five filters and converts
grey, RGB, palette, grey with alpha and RGBA samples at 8 bits to
`0xAARRGGBB`; interlaced and 16 bit images are rejected. `image_load`
reads a file. `painter_image` blends with straight alpha.
`tools/genicons/genicons.py` writes PNG files without external
libraries: the 16x16 icons in `user/share/icons/` (installed under
`/usr/share/icons/`, cached per process by `icon_get`, which prefers
an SVG file of the same name; see `icons.md`) and the test
images in `lib/libgui/tests/data/`, whose rows cycle through all filter
types and whose pixels follow formulas the tests recompute.

### Popups and tooltips (`src/window.c`)

A `floating` widget is skipped by layout and positioned by its owner.
`window_popup_open` adds one as the last child of the window at
absolute coordinates, so it is painted last and hit first; a click
outside it or Escape closes it (`window_popup_close` destroys it).
Combo boxes, menus and tooltips use it. A widget with `widget_set_tip`
shows a tooltip label 600 ms after the pointer enters it, through an
application timer; any press, key or leave hides it.

### Controls and containers (`src/widgets/controls.c`, `containers.c`)

Combo box (`changed`; popup list, wheel and arrow keys), spinner
(`changed`; arrows, keys, wheel), slider (`changed`; thumb dragging,
keys), progress bar; tabs (`changed`; the header strip selects, pages
are vertical boxes hidden unless current), split pane (two children,
draggable divider), tool bar (a horizontal box of icon buttons with
tooltips) and status bar (a horizontal box of labels). Buttons and
labels gained `widget_set_icon`.

### Menus (`src/widgets/menu.c`)

`menubar_new`, `menu_new` (an invisible widget containing items) and
`menu_add` / `menu_add_separator`. Opening a title creates a drop down
popup that paints the items, tracks hover, activates on click or Enter
(`clicked` on the item), moves between menus with Left and Right, and
closes with Escape or an outside click. Up and Down skip separators and
disabled items. Titles and items underline their mnemonic.

### Data views (`src/widgets/models.c`, `gui/model.h`)

`struct model` supplies row counts and children by row id, columns,
cell text, headers and an optional sort. Both views flatten the visible
rows on demand (the tree view following its set of expanded row ids),
draw only the rows in view and retain an internal scroll bar. The tree
view draws expanders and handles Left, Right and expander clicks; the
table draws a header row, sorts on header clicks through the model and
resizes columns by dragging the header borders. `view_refresh` is
called after the model changed. A row pressed and moved past the drag
threshold emits `drag_begin`, and drags over a view emit `drag_motion`,
`drop` and `drag_leave` with the row under the cursor (`dnd.md`).

### Editor (`src/widgets/editor.c`, `editor_hl.c`)

Lines are separate strings. Edits go through `insert_text` and
`delete_range`, which record inverse operations; consecutive typed
characters merge into one undo step, a newline starts a new one, and
undo and redo move operations between the two stacks. Word wrap
computes visual rows per line at the last space that fits, line
numbers add a gutter, `editor_find` searches forward or backward from
the cursor with wrap around and selects the match, and the clipboard,
selection with Shift and the mouse, Ctrl+Z and Ctrl+Y follow the text
field conventions. A highlighter fills a class per character for each
line with a state carried across lines; `highlight_c` covers keywords,
strings, numbers, line and block comments and preprocessor lines,
`highlight_sh` keywords, strings, variables and comments. The selection
is dragged as `text/plain` from a press inside it, and text dropped on
the editor is inserted at an accent drop caret; the text field drags its
selection as well (`dnd.md`).

### Dialogs and applications

`app_dialog` and `app_prompt` (`src/dialog.c`) open a second window and
run nested `app_step` calls until a button is chosen. `app_run_command`
(`src/command.c`) runs a program with an input and collects its output
through a watched pipe in the same nested loop, so the windows continue
to paint while the caller waits. `app_run_privileged` runs it as root
through `sudo -A` and the authentication dialog (`users.md`).
`painter_avatar` draws the avatar of an account for the greeter and that
dialog. `gui_set_translucent` gives a window ARGB buffers, and X12 blends
its pixels outside the opaque region with their alpha. `app_choose_file`
(`src/filechooser.c`) is the file chooser for Open and Save, built on the
folder view that the file manager shares (`folderview.md`).
`app_choose_program` (`src/appchooser.c`) is the application chooser for
Open with in Files and on the desktop. A tree lists the applications of
the launcher tables (`gui/launcher.h`) in two groups. Recommended
applications contains the commands of `mime_handlers` for the type of
the file. The default handler is first and selected. A handler without
a launcher entry appears under the file name of its program. Other
applications contains the remaining entries in the order of their
titles. Entries with an `@` command and duplicate commands are left
out. The check box Always use makes the chosen command the handler of
the type and saves the user's handler table. Other program opens the
file chooser in `/usr/bin`. An executable regular file from it is added
at the top of Other applications and selected. The caller starts the
chosen command with `mime_run`. The applications
moved to the framework: `term` (a canvas over a watched pseudo terminal
descriptor), `files` (a table over a directory model, tool bar, status
bar), `view` (a read only editor with menus and a tool bar), `gedit`
(the editor with menus, a tool bar, search, save with Ctrl+S, a
close confirmation), `clock`, `paint`, `pong` and `widgettest`. The
M19 toolkit (`gui/widgets.h`) was removed.

### Tests

`make check` gained PNG decoding against the generated images (all
filters, colour types, a stored deflate block, corrupt and missing
files, alpha blending of an icon), controls (combo popup, spinner,
slider), containers (tabs, split pane, menus), models (expansion,
selection, sorting, column resizing) and the editor (typing, undo and
redo groups, cut and paste across lines, search, wrapping,
highlighters), plus the gedit widget tree. Boot tests: `icons` (icon
decoding in the target), `gui_widgets` (rewritten for the framework
client), `gui_controls` (combo popup, spinner, slider, tabs through
injected input) and `gui_editor` (typing C source into gedit, keyword
colour on screen, Ctrl+S writes the file).

## High density outputs (M33)

Windows render at the output's integer scale: `gui_window.scale` device
pixels per logical pixel, buffers with `set_buffer_scale`. `struct
painter` carries the scale, so widget code continues drawing in logical
pixels while text is rasterized at `px * scale` and lines are `scale`
pixels thick; see `display.md`. Only code that writes into
`gui_window.surf` directly sees device pixels.

## Editing commands and context menus

The editor widget has the functions `editor_has_selection`,
`editor_selection`, `editor_cut`, `editor_copy`, `editor_paste`,
`editor_delete_selection`, `editor_select_all`, `editor_can_undo`,
`editor_can_redo` and `editor_replace_all`. Ctrl+C, Ctrl+X, Ctrl+V and
Ctrl+A call the same functions. The clipboard is the selection of the
X12 data device (`gui_clipboard_set`, `gui_clipboard_get` in
`src/client.c`). Both the editor and the text field already used it
before this change.

An operation records an undo group number in `struct op`. Operations
with the same nonzero number are undone and redone together. A paste
that replaces a selection, a newline with indentation, the indentation
of several lines and `editor_replace_all` are each one undo step.

Ctrl+Left and Ctrl+Right move the cursor by words, and Ctrl+Backspace
and Ctrl+Delete delete to the start or the end of a word. A word consists
of letters, digits, underscores and bytes of multibyte characters. Home
moves to the first character of the line that is not a space or a tab,
and a second Home moves to column 0. Enter starts the new line with the
spaces and tabs that start the current line before the cursor. Tab with
a selection over several lines inserts four spaces at the start of each
line, and Shift+Tab removes up to four leading spaces from the selected
lines or from the line of the cursor. A second click within 400 ms at the
same place selects the word, and a third click selects the line. The
line of the cursor has a faint background while the editor has the
focus and no selection.

A right click on an editor or a text field opens a context menu
(`src/widgets/editmenu.c`). The menu of the editor contains Undo, Redo,
Cut, Copy, Paste, Delete and Select all, and the menu of a text field
contains the same items without Undo and Redo. A click outside the
selection first moves the cursor to the click. A click inside the
selection does not change the selection. Items that do not apply are disabled. The menu is an
invisible child of the widget and is destroyed with it. The handler
argument of an item is its action, and the menu stores the callback of
the widget in `user`.

A drop down menu draws the accelerator of an item, such as Ctrl+S, at
its right edge. Disabled buttons and menu items draw their icon with 40
percent of its opacity (`icon_dimmed`).

gedit has File and Edit menus with accelerators. The Edit menu contains
Undo, Redo, Cut, Copy, Paste, Delete, Select all, Find, Find next (F3),
Replace (Ctrl+H, replaces every occurrence) and Go to line (Ctrl+L). The
tool bar contains New, Open, Save, Undo, Redo, Cut, Copy, Paste and
Find, followed by the Wrap and Line numbers check boxes. The commands
that do not apply are disabled. The status bar shows a message, the
language, the cursor position and the number of lines. Lua files are
highlighted with `highlight_language_lua`. New, Open, Quit and closing
the window ask whether unsaved changes are saved.

`make check` tests word movement, word deletion, indentation, Home, Tab
and Shift+Tab with their undo steps, `editor_replace_all`, the clipboard
functions, the double click and Cut in the context menu. The boot test
`gui_editor` selects the typed text with Ctrl+A, copies it with the
context menu and pastes it on a second line.

## Graphs, colours and display modes (X1 of `docs/plan/x12settings.md`)

- `graph_new` (`src/widgets/graph.c`) draws up to four time series. The
  widget retains the last samples of each series, the newest at the
  right edge. A heading row shows the title and either a value text or
  the legend. A framed plot with a grid of four rows shows each series
  as a line (`GRAPH_LINE`), as a line with the area under it
  (`GRAPH_AREA`), or only as a legend text (`GRAPH_TEXT`). The scale is
  fixed, or with automatic scaling the largest retained sample rounded
  up to 1, 2 or 5 times a power of ten (`graph_nice_max`). A format
  function writes the scale into the top left corner of the plot. The
  Resources tab of `sysmon` consists of these graphs. They replaced its
  private drawing code.
- `gfx_rgb_to_hsv`, `gfx_hsv_to_rgb` and `gfx_color_parse` in `gui/gfx.h`
  convert colours. The round trip through HSV changes a channel by at
  most 3. `make check-libgui` checks every colour.
- `color_dialog` (`src/widgets/color.c`) is a modal dialog with a field
  of saturation and value, a hue strip, the old and the new colour, and
  the hexadecimal value. A typed value is retained exactly, not in its
  rounded HSV form. `colorbutton_new` shows a swatch with the
  hexadecimal value, opens the dialog and emits "changed". The desktop
  colour of the Settings program uses the button instead of three
  sliders.
- `gui/display.h` contains the packed form of a display mode, the
  conversions `display_mode_parse` and `display_mode_format`, and
  `display_resolutions`, the modes of a virtio-gpu scanout. X12, the
  desktop, the Settings program and `x12settings` share it.
