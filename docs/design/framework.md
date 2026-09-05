# Application framework

M21 adds a retained widget framework to `libgui/` beside the M19
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
iteration means a burst of input costs one repaint.

`app_create_detached` (used by the host tests) builds an application
over the fake client in `tests/fake_client.c` with the builtin font.

## Widget model (`src/widget.c`, `src/window.c`)

- `struct widget` holds the class pointer, tree links, geometry inside
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
  returns non zero. Each signal name has a fixed argument structure
  (`sig_click`, `sig_change`, `sig_key`, `sig_select`, `sig_resize`,
  `sig_scroll`, `sig_paint`).
- Windows translate server messages: mouse messages go to the widget
  under the cursor or to the capturing widget (`widget_capture`, held
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
`widget_set_min`, `widget_set_max`), keeping preferred sizes inside the
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

`struct theme` holds named colours (`TC_*`) and metrics (`TM_*`) at
scale 100, a scale in percent, and the font. `theme_apply` loads the
outline font at `TM_FONT_PX` scaled (DejaVu Sans at 14 pixels by
default, the builtin 8x16 font when the file is missing);
`app_theme_changed` reapplies it and relayouts every window.

`struct painter` wraps a surface with an origin and a clip rectangle
stack: `painter_push(x, y, w, h)` enters a child area, so widgets paint
in local coordinates and every primitive (`fill`, `frame`, `line`,
`rounded`, `text`, `blit`, `mask`, `focus_ring`) is clipped to the
widget. Text uses the theme font.

## Partial redraw

`widget_invalidate` marks a widget dirty and its ancestors child dirty.
`window_paint` relayouts when needed (which marks the whole window
dirty), then walks the tree: a dirty widget and everything below it are
repainted, a widget with only dirty descendants recurses without
painting. The union of the repainted rectangles is sent to the server
in one `gui_damage`; `app_set_damage_log` prints it for the tests.

## Core widgets (`src/widgets/`)

label, button (`clicked`), check box and radio button (`toggled`, radios
exclusive among siblings), separator, canvas (`paint`, `press`,
`release`, `motion`, `wheel`, `key`), text field (`changed`,
`activate`; cursor, selection with Shift and the mouse, Ctrl+A, C, X,
V through the server clipboard), list view (`selected`, `activate`;
keyboard navigation, wheel, internal scroll bar), scroll bar
(`scrolled`; thumb dragging, paging, wheel, keys) and scroll area (a
viewport over its content box, `w->user`, with bars shown when the
content overflows).

## Tests

- `make check` compiles libgui (without `client.c`) and libfont with
  the host compiler against `tests/fake_client.c` and runs
  `tests/test_framework.c`: box and grid layout, signal order and
  consumption, partial redraw rectangles, focus traversal, mnemonics,
  clicks, text editing with cut and paste, list navigation.
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
images in `libgui/tests/data/`, whose rows cycle through all filter
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

`menubar_new`, `menu_new` (an invisible widget holding items) and
`menu_add` / `menu_add_separator`. Opening a title creates a drop down
popup that paints the items, tracks hover, activates on click or Enter
(`clicked` on the item), moves between menus with Left and Right, and
closes with Escape or an outside click.

### Data views (`src/widgets/models.c`, `gui/model.h`)

`struct model` supplies row counts and children by row id, columns,
cell text, headers and an optional sort. Both views flatten the visible
rows on demand (the tree view following its set of expanded row ids),
draw only the rows in view and keep an internal scroll bar. The tree
view draws expanders and handles Left, Right and expander clicks; the
table draws a header row, sorts on header clicks through the model and
resizes columns by dragging the header borders. `view_refresh` is
called after the model changed.

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
`highlight_sh` keywords, strings, variables and comments.

### Dialogs and applications

`app_dialog` and `app_prompt` (`src/dialog.c`) open a second window and
run nested `app_step` calls until a button is chosen. The applications
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
painter` carries the scale, so widget code keeps drawing in logical
pixels while text is rasterized at `px * scale` and lines are `scale`
pixels thick; see `display.md`. Only code that writes into
`gui_window.surf` directly sees device pixels.
