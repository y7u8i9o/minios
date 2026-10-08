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

## Metrics and shapes (K4)

The theme has the metrics `TM_RADIUS` (6), `TM_ICON` (16), `TM_ROW_PAD`
(6) and `TM_INDENT` (16) at scale 100. `theme_scale_px(t, px)` scales the
remaining sizes, such as the padding of text fields, the arrows of combo
boxes and spinners, the slider thumb, the tab and menu padding, the
divider of split panes, the table header, the colour field and the
minimum thumb of scroll bars. All of them follow `ui_scale`. The colours
`TC_BUTTON_CHECKED` and the header colours of the client side
decorations (`TC_HEADER` to `TC_HEADER_BUTTON_BACKDROP`) are part of the
theme. `csd.c` takes its colours from the theme of the application.

`paint.c` draws the antialiased shapes. No widget and no program contains
shape code of its own.

| Function | Shape |
|---|---|
| `painter_round_rect(p, x, y, w, h, r, fill, border)` | rounded rectangle, corners from `pixel_corner_table` |
| `painter_rounded` | the same with the theme radius |
| `painter_disc`, `painter_ring` | disc and ring from `pixel_disc_table` |
| `painter_stroke` | polyline with the coverage of the distance from the line |
| `painter_check`, `painter_chevron` | check mark and chevron on `painter_stroke` |
| `painter_fill_alpha` | translucent fill |
| `painter_focus_ring` | rounded ring in the accent colour |

The shapes are drawn in device pixels, so that edges at scale 2 are
sharp. `screenshot` draws its discs and rounded rectangles with these
functions. `lib/libgui/tests/test_paint.c` compares corners with the
table at scale 1 and 2, checks the symmetry of chevrons, the extent of
the check mark and the end points of a stroke, and the metrics at
`ui_scale` 150. The boot case `gui_widgets_look` finds an antialiased
pixel at the corner of a text field and takes a screendump.

## The look of the widgets (K5)

- Check boxes are rounded boxes with a painted check mark, and radio
  buttons are discs with a dot. A checked control is filled with the
  accent colour, and its mark has the selection text colour.
- Chevrons replace the drawn arrows of the combo box, the spinner, the
  tree expanders and the sort mark of tables.
- Check boxes, radio buttons, combo boxes, spinner arrows, the slider
  thumb, tabs, menu bar titles, table headers and rows show the pointer.
  A hover move between rows repaints the two rows. Spinner arrows and
  table headers show the pressed state.
- Every widget has a disabled look: the track colour as background and
  the disabled text colour. A disabled widget ignores input.
- Selected and hovered rows of lists, trees and tables are rounded pills.
  Icons on selected rows take the selection text colour through
  `icon_variant`. Rows are the font height plus `TM_ROW_PAD` high, and
  menu items are `TM_CONTROL_H` high.
- The editor and the text field draw the selection in `TC_SELECTION` and
  the selected text in `TC_SELECTION_TEXT`. The current line of the
  editor is a `pixel_blend` of the highlight colour over the field. The
  syntax colours are the table `highlight_colors` in `editor_hl.c`.
- The progress bar is as high as the text of its percentage.
- Tooltips are the class `tooltip_class` of `window.c`, a rounded box
  whose padding follows the theme.

`lib/libgui/tests/test_look.c` checks the hover damage of two rows, the
disabled look and the pixels of the check mark, the radio dot and the
chevron. `gui_widgets_look` checks the marks on the screen, and the
`hover` scenario of `gui_bench` damages exactly two rows per step.

## Editing functions and new widget functions (K6)

- The text field records snapshots of its text and cursor before each
  edit. Ctrl+Z undoes a step, and Ctrl+Y redoes a step. The context menu
  has the entries Undo and Redo. Typed characters in a row form one
  step. The history has at most 100 steps. A masked field records no
  history.
- `gui_word_left`, `gui_word_right` and `gui_word_at` in `utf8.c` find
  word boundaries. The text field and the editor use them for Ctrl+Left
  and Ctrl+Right, also with Shift, and for the double click.
- A double click on a text field selects a word. A triple click selects
  the whole text. `textfield_set_placeholder` sets a text that an empty
  field shows in the disabled text colour.
- The caret blinks with the period `GUI_CARET_BLINK_MS` (530 ms).
  `framework.md` describes the blink. The terminal uses the same period.
- The spinner accepts typed digits and a minus sign for a negative range.
  Enter or a loss of the focus commits the typed value, clamped to the
  range. Escape restores the previous value.
- `combobox_set_editable` adds a text field to a combo box. An edit emits
  `changed` with the index -1. A choice from the list sets the field.
- `label_set_wrap` wraps the text of a label at its width with
  `painter_wrap`. The label measures its height again when the line
  count changes. `label_set_ellipsis` ends a text that does not fit with
  an ellipsis.
- `progress_set_pulse` turns a progress bar into an indeterminate bar. A
  block moves along the bar in 40 steps of 50 ms.
- `menuitem_set_check` and `menuitem_set_radio` turn a menu item into a
  check item or a radio item. `menuitem_checked` returns its state. A
  click on a check item toggles the item. A click on a radio item checks
  the item and clears the other radio items of its menu.
  `menu_add_submenu` adds an item with a submenu. The drop down shows the
  submenu in a second column. Right and Enter open the submenu, and Left
  closes it. The View menu of Files has a check item for hidden files and
  radio items for the sort order.
- `imageview_new` and `imageview_set` show an image centred in the
  widget. A larger image is reduced to fit with its proportions. A
  smaller image is never enlarged. The reduced image comes from
  `image_scale` and is cached per size and scale. The Lua binding uses
  the class.

`lib/libgui/tests/test_functions.c` checks each function. The boot case
`gui_widgets_text` runs `widgettest text`. The kernel test types into a
text field, undoes and redoes the text, moves the cursor with Ctrl+Left,
selects a word with a double click, types a value into a spinner,
toggles a check menu item and chooses an item of a submenu. The expect
file checks the log of the client.

## Classes of full screen windows (K7)

`src/widgets/backdrop.c` contains the classes of the greeter, the screen
locker and askpass.

- `spacer_new(parent)` takes free space with the stretch 1 in both
  directions and paints nothing.
- `backdrop_new(window, style)` is a vertical box that fills the window.
  The function removes the padding of the window. `BACKDROP_DESKTOP`
  paints the wallpaper of `/etc/desktop.conf` or a gradient of its
  desktop colour. The backdrop renders the wallpaper at its device size
  and renders it again after a change of that size. `BACKDROP_DIM` paints
  black at half opacity.
- `card_new(parent, width)` is a vertical box of the given width with
  rounded corners and a padding of 16 pixels, centred in its parent. In a
  window after `window_set_translucent` the card sets the opaque region
  of the window to three rectangles. The rectangles cover the card
  without its rounded corners. Escape in the card emits `cancel`.
- `account_new(parent, name, full_name, focusable)` shows the avatar, the
  full name and the account name in a row of `ACCOUNT_ROW_H` (52) pixels.
  A focusable row shows a hover pill and a selection pill. A click, Enter
  or Space emits `clicked`. Up and Down move the focus between the
  focusable rows of one parent. `account_set` changes the account of a
  row, and `account_name` returns the account name.
- `toolbar_set_center(toolbar, w)` centres the child `w` on the whole
  tool bar. The top bar of the greeter and of the screen locker centres
  its clock with this function.
- A box with the flag `transparent` paints no background.

The greeter and the screen locker use the classes through
`user/greeter/screen.c`. askpass uses the backdrop `BACKDROP_DIM`, the
card and an account row. `lib/libgui/tests/test_functions.c` checks the
opaque region of the card, the centred clock, a click and Enter on an
account row and the move of the focus with Down.

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
