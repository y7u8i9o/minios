# Widgets

`docs/plan/widgets.md` revises the widgets of libgui in eight
milestones, K1 to K8. This document describes the widgets as
implemented. `framework.md` describes the widget tree, the layout and the
paint pass. `gui.md` describes the client library.

## Measurement at the scale of the window (K1)

A window on a high density output has `scale` device pixels per logical
pixel. The painter rasterizes text at the font size times the scale.
Hinting and kerning work in device pixels. The width of a text at scale 2
is therefore not always twice its width at scale 1. Every width and
every hit test of a widget uses the scale of its window.

- `widget_scale(w)` returns the scale of the surface that shows `w`. A
  widget in a popup surface has the scale of the popup. A widget without
  a window has the scale 1.
- `widget_text_width(w, font, text, n)` returns the width of the first
  `n` bytes in logical pixels, rounded up. `widget_text_index(w, font,
  text, n, px)` returns the character boundary nearest to the logical
  offset `px`. A NULL font is the font of the theme.
- `painter_text_width_font` and `painter_text_index_font` give the same
  values in a paint function for a font other than the theme font.
- `gfx_text_width_font` and `gfx_text_index_font` measure at scale 1.
  They remain for surfaces without a scale. No widget calls them.

The text field, the combo box, the menus, the tabs, the tooltips, the
drag image, the message dialog, the path bar of the folder view and the
editor measure with these functions. The editor places each run of
highlighted text at the width of the row up to the run. A sum of the
rounded widths of the single runs would drift by a pixel per run. The
gutter of the editor has the width of the line count in zeros.

A text field never scrolls further than the end of its text. A shorter
text therefore scrolls back into view.

## Repaints without a change (K2)

A widget is invalidated only when its pixels change. A wheel step or a
page click at the end of a range causes no paint. `scroll_set` reports
whether a value changed. `scrollbar_set`, `widget_set_enabled`, the
selection of the selected row, Home and End without a move, and a slider
release without a press repaint nothing. The programs `hexview`,
`unicode` and `view` repaint only when the view moved. The local
relayout is described in `framework.md`. `lib/libgui/tests/test_perf.c`
checks the ends of a table and a list view, a status label update and a
scroll step of a scroll area.

## Partial repaints and the editor (K3)

The text field repaints from the edited column to its end, and a caret
move repaints the old and the new caret column. A change of the scroll
offset or a selection repaints the field. The editor wraps only the
edited lines again. The row index `first_row` gives the rows of a line.
The states of the highlighter at the line starts are cached up to the
first edited line, and a paint runs the highlighter from the first
painted line. An edit inside one line repaints the rows of that line,
and a cursor move repaints the old and the new cursor row. The tree view
and the table map row ids to flat rows with a hash map
(`lib/libgui/src/intmap.c`) and record the expanded rows in a second
map. The tabs and the menus measure their title widths once per
measurement. `lib/libgui/tests/test_partial.c` checks the damage of a
keystroke, the pixels after scrolling by copy at scale 1 and 2, the
calls of `cell` per wheel step, the highlighter runs and the shapings of
one keystroke.

The counter `text_shapes` shows no shaping of the same text twice in a
paint, so the plan's cache of shaped text is not added.

## Mnemonics

A caption marks its mnemonic with `&` before the character.
`painter_mnemonic_strip` removes the marker and returns the offset of the
mnemonic character. `painter_mnemonic_text` draws the caption without
the marker and underlines the character. Buttons, labels, check boxes,
radio buttons, menu titles and menu items use these functions.

## Scroll tracks

`scrollbar_thumb(len, value, max, page, &off, &len)` gives the thumb of a
track of `len` pixels. The thumb lies 2 pixels inside both ends of the
track and is at least 8 pixels long. The paint and the hit test of the
scroll bar use this geometry. Before K1, the hit test used a track 2
pixels longer than the painted one.

`scroll_clamp` limits a value to the range from 0 to `max - page`.
`scroll_set` stores a clamped value and reports a change.

The list view, the tree view, the table and the editor have a scroll
track inside the widget. `scroll_track_event` handles the mouse events of
such a track. A press before or after the thumb moves one page. A press
on the thumb captures the mouse and drags the thumb. `struct
scroll_track` stores the offset of the press inside the thumb.

## Clicks

`gui_click_count` counts repeated presses. A press repeats the previous
press when it follows within `GUI_DOUBLE_CLICK_MS` (400 ms) and lies
within `DRAG_THRESHOLD` (4 pixels). The count runs 1, 2, 3 and then
starts again at 1. The list view, the tree view, the table, the editor
and the terminal use the function. The header bar of the client side
decorations uses `GUI_DOUBLE_CLICK_MS` for its double click.

A double click on a row of the list view emits `activate`, as on the
tree view and the table.

## Menus

The arrow keys in an open menu skip separators and disabled items. A
menu with no item that can be chosen leaves the highlight unchanged. The
names of the accelerator keys (`Ctrl+`, `Alt+`, `Shift+`, `Del`) are
translated.

## Canvases

A canvas is not focusable by default. A click on a canvas therefore
leaves the focus in the text field or list that had it. A program whose
canvas takes keys sets `focusable`. The programs `calc`, `evtest`,
`sequencer`, `pong`, `mandel`, `player`, `unicode`, `synth`, `term`,
`view`, `hexview` and `paint` do so. The Lua binding sets the flag when
a script connects `key` or `keyup` to a canvas.

## Colour button and colour dialog

The colour button acts on the release of the left button over it, as the
button class does. The button shows the pressed colour from the press to
the release. The colour dialog computes its field of saturation and
value in device pixels at the scale of the painter.

## Icons

The icon cache is described in `icons.md`. Widgets draw icons with
`painter_icon`. The function takes the rendition of the icon at the scale
of the painter.

## Translated strings

libgui translates its own strings in the `libgui` domain.
`lib/libgui/src/intl.h` defines `_()`, `N_()` and `ngettext` for that
domain. The catalogues are in `user/po/libgui/`.

## Tests

`lib/libgui/tests/test_scale.c` runs the fake client at scale 2 with
DejaVu Sans. The test checks the following:

- The caret of a scrolled text field lies at the end of the field.
- The caret of a short text lies at the painted width of the text.
- A click at a glyph boundary puts the cursor at that boundary.
- The measured widths of a combo box and a menu item equal the painted
  widths.
- A click one pixel left of a tab edge and a click at the edge select
  the two adjacent tabs.
- The painted thumb of a scroll bar starts at the offset of
  `scrollbar_thumb`. A press on its top row grabs it, and a press below
  it pages.
- The colour button shows the pressed colour.
- A menu item "&Open" shows no marker and underlines the O. Down skips a
  separator and a disabled item.
- A double click on a list row emits `activate`.
- The icon cache returns renditions at scale 1 and 2 and in two colours.

The boot case `gui_widgets_scale2` runs `widgettest scale` at
`video=2048x1536@2`. widgettest places the boundary between "Wid" and
"gets" of a text field at x 200 of its window. The kernel test clicks
there and types x. The log must show "Widxgets".
