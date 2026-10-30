# Cursors

The pointer cursors are SVG files in `/usr/share/cursors`. A client asks
for a cursor by its name, and X12 renders the file at the screen scale.
Before 2026-10-30, X12 had a single arrow of 12 by 18 pixels as character
art in `scene.c`, enlarged by nearest neighbour sampling at scale 2, and
no client set a cursor.

## The set

`tools/gencursors.py` writes the files and the file `index`. The shapes
are designed on a grid of 24 by 24 units, and the script scales them into
a view box of 20 by 20 units, one unit per logical pixel. The outline
remains one unit wide after the scaling. A cursor consists of groups. Each
group is drawn first as a white halo, the shapes filled and stroked one
unit wider on every side, then as the black body. A later group lies over
an earlier group with its own halo, so a badge remains readable over the
arrow. White detail lines separate the fingers of the hands. The lens of
the zoom cursors is white glass without a halo. The tip and the left edge
of the arrow lie on whole units, so the outline covers whole pixels at
scale 1. The first set of 2026-10-30 had cursors of 24 pixels. The owner
found them too large on the same day.

The names are those of the CSS cursor property:

- `default`, `pointer`, `text`, `crosshair`, `move`, `all-scroll`, `cell`
- `grab`, `grabbing`
- `wait`, `progress`, `help`, `context-menu`, `not-allowed`
- `copy`, `alias`, `no-drop`
- `ew-resize`, `ns-resize`, `nwse-resize`, `nesw-resize`, `col-resize`,
  `row-resize`
- `zoom-in`, `zoom-out`

The edge names `n-resize`, `s-resize`, `e-resize`, `w-resize`,
`ne-resize`, `nw-resize`, `se-resize` and `sw-resize` are aliases of the
four resize cursors. The index has one line `NAME X Y` per cursor with the
hotspot in view box units, and one line `NAME = TARGET` per alias. The
files belong to the package `theme`.

The cursors need strokes. The SVG renderer of `svg.so` draws `stroke`,
`stroke-width`, `stroke-opacity`, `stroke-linejoin`, `stroke-linecap` and
`stroke-miterlimit` since the same change (`codecs.md`).

## Rendering in X12

`cursor.c` reads the index at start. A shape is an entry of the index, and
an alias refers to the entry of its target. Shape 0 is `default`.
`cursor_image_of` renders the file of a shape with `image_load_svg` at 20
logical pixels times the screen scale. A soft shadow follows: the alpha of
the cursor, moved down by one logical pixel, blurred twice with a box of
the scale as radius, at 30 percent. The image has a border of three
logical pixels for the shadow. The image is 26 by 26 logical pixels, 52 by
52 device pixels at scale 2, and fits the device cursor of virtio-gpu (64
by 64). The virtio-gpu driver sends the device the top left corner of the
image, because the cocoa window of QEMU ignores the hotspot
(`display.md`). X12 renders each shape once per scale. Without the index or the
file of `default`, X12 draws the former arrow. A shape whose file fails
shows `default`.

## The shown cursor

The seat (`seat.c`) combines three sources. The first source in this
list that applies decides:

1. A cursor of X12 itself, the override, recomputed after every pointer
   event (`seat_cursor_update`):
   - During a drag and drop: `copy`, `default` for a move, and `no-drop`
     over a target that accepts no action (`data_cursor` in `data.c`).
   - From the window manager module (`wm_cursor`): `grabbing` during a
     move of a window, the resize cursor of the edges during a resize and
     over the edges of server decorations, `nwse-resize` over the grip,
     `default` over the title bar, and `wait` over the contents of an
     unresponsive client.
2. A cursor surface of the client under the pointer (`pointer.set_cursor`).
3. The shape of the client under the pointer (`pointer.set_shape`).

A locked session has no override. When the pointer enters another surface,
the cursor of the previous client ends: the shape returns to `default`, and
the cursor surface no longer applies.

## The protocol

`pointer.set_shape(serial, name)` is version 2 of the pointer and needs a
seat of version 3. serial is the serial of the last enter event. An unknown
name shows `default`. `set_cursor` with a surface remains for cursors that
a client draws itself.

## libgui

The client part (`client.c`) sends `set_shape` when the cursor changes, and
only to a seat of version 3. The cursor depends on the zone under the
pointer: the resize cursor over the edges of client side decorations, the
cursor of the contents, or `default` over the header. `gui_resize_cursor`
in `gfx.c` maps the edges of `gui_resize_edges` to the four resize names,
and the window manager module of X12 uses the same function. During
`app_run_command`, `gui_set_busy` makes every window show `progress`.

The widgets decide the cursor of the contents. `widget_cursor_at` walks
from the widget under the pointer to the window. The first of these
decides: a disabled widget shows `default`, then the `cursor_at` function
of the class, the `cursor` field of the widget (`widget_set_cursor`), and
`text` for a widget that accepts text. The window sends the result after
every pointer motion, press and release (`window_update_cursor`).

- Text fields, the editor and the terminal show `text`.
- The divider of a split pane shows `col-resize` or `row-resize`, also
  during its drag.
- The canvas of `paint` shows `crosshair`.

## Log

X12 writes `N cursors` when it reads the index. In verbose mode, X12
writes `cursor NAME` for every change of the shown cursor, `cursor
surface` for a cursor surface and `cursor hidden` for a hidden cursor.

## Tests

- `gui_cursors`: `ew-resize` over the right edge of a window with server
  decorations, `grabbing` during a move, `text` over the text field of
  `apptest`, and `default` over the desktop.
- `comp_cursor`, `gpu_mode`, `gui_pointer`, `gui_tablet`, `input_tablet`
  and `gui_scale2` check the default cursor at the pointer: the black body
  and the white outline in the framebuffer, or the size, the hotspot and
  the pixels of the device cursor (`arrow_cursor_at` in
  `kernel/tests/gui_helpers.h`).
- The host check of libgui tests the cursors of a text field, a button, a
  widget with `widget_set_cursor`, a disabled field and a split pane, and
  the strokes of the SVG renderer.
