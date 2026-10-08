# Widgets: correctness, performance, look, missing functions and shared classes

Most widgets of libgui date from M21 and M22 of `m19-m26-display.md`:
`lib/libgui/src/widgets/`, `widget.c`, `window.c`, `layout.c` and
`paint.c`. Newer code (`csd.c`, `folderview.c`, `appchooser.c`,
`filechooser.c`, `graph.c`) measures text at the scale of the window,
draws antialiased shapes from coverage tables, shows hover feedback and
rounded highlights, and translates its strings. The old widgets do none
of this. Several of them also repaint without a change. A wheel step at
the end of a list, a table or the editor repaints the whole widget
(`listview.c:105-108`, `models.c:364-367`, `editor.c:1140-1147`). Every
`widget_relayout` marks the window. `window_paint` then measures, lays
out and repaints the whole window (`widget.c:273-278`,
`window.c:82-89`). A status bar update or a clock tick therefore
repaints the whole window.

This plan brings the widgets to the level of the newer code, removes
the repaints that change no pixel, and moves the private widget classes
of the greeter, the screen locker, askpass and the panel into libgui.
Each milestone below ends with boot tests in `tests/cases/` and is
marked completed here when they pass. A milestone that changes
performance records its numbers before and after.

## 1. Scope

The owner chose the following scope on 2026-10-09. Performance was
added on the same day as a primary focus.

- **Correctness:** text measured at the scale of the window, one
  geometry for painting and hit testing of scroll bars and tabs,
  missing states, menu navigation, mnemonics in menus, double clicks
  in lists, icons at the scale of the window, translated widget
  strings and corrected documents.
- **Performance:** no repaint without a change, a relayout limited to
  the changed part, partial repaints of a widget, scrolling by copy,
  and an editor whose work per edit depends on the changed lines.
- **Look and consistency:** antialiased corners, marks and arrows,
  round radio buttons, hover and pressed states, a disabled look in
  every widget, rounded selection rows, and sizes that follow
  `ui_scale`.
- **Missing functions:** undo, word movement, word selection,
  placeholder and caret blink in the text field. An editable spinner,
  an editable combo box, label wrap and ellipsis, an indeterminate
  progress bar, check and radio menu items, submenus, an image widget
  and a tooltip class.
- **Shared classes:** the private classes of the greeter, the screen
  locker and askpass become libgui classes. The panel draws with the
  paint functions of the libgui classes.

Outside the scope:

- Keyboard access to controls: F10 for the menu bar, arrow keys in
  radio groups, keyboard operation of split panes and scroll areas,
  type-ahead in lists. Word movement in a text field is text editing
  and belongs to the scope.
- Multiple selection in lists and tables. No program needs it.
- The toolbar toggles of `screenshot` and the icon grid of the
  desktop. Their private copies of shared functions are still
  replaced (§2).
- Ctrl+Backspace in the text field.

Owner decisions of 2026-10-09:

- The header bar of the client-side decorations is 30 px
  (`csd.h:10`). `docs/design/gui.md:339` and `CLAUDE.md` state 36 px
  and are corrected in K1.
- Controls remain 26 px high (`TM_CONTROL_H`). The rule of 20 px
  buttons applies to the round buttons of the header bar. The corner
  radius `TM_RADIUS` changes from 5 to 6 px.
- Canvases are not focusable by default. A program whose canvas takes
  keys sets the flag.
- The caret blinks for 10 s after the last input. After that period
  the caret is visible without repaints.

## 2. Fixed decisions

- **Scaled measurement.** Each width and each hit test of a widget
  uses the scale of its window. `paint.h` gains
  `painter_text_width_font` and `painter_text_index_font` for fonts
  other than the theme font. `widget.h` gains `widget_scale`,
  `widget_text_width(w, font, text, n)` and
  `widget_text_index(w, font, text, n, px)`. A NULL font means the
  theme font. The functions return the same logical widths as
  `painter_text_width`. `gfx_text_width_font` and
  `gfx_text_index_font` remain for surfaces at scale 1. No widget calls
  them.
- **Counters.** Performance is measured with deterministic counters.
  Times are printed and never asserted, as in §2 of
  `compositor-performance.md`. `struct gui_stats` gains
  `widget_paints`, `painted_pixels`, `layouts` and `text_shapes`. The
  fake client of the host tests gains a scale and a sum of damaged
  pixels. `compbench` gains widget scenarios. The new boot cases
  `gui_bench` and `gui_bench_hidpi` run them. `tools/benchtable.py`
  prints the tables.
- **No change, no paint.** A widget is invalidated only when its pixels
  change. A wheel step or a page click at the end of a range causes no
  paint and no commit. One shared helper clamps a scroll value and
  reports a change. The helper replaces the clamping copies in the
  widgets and in the programs.
- **Partial invalidation.** `widget_invalidate_rect` invalidates a part
  of a widget. A widget with the new bit `transparent` paints no
  background of its own. Its invalidation repaints the area of the
  nearest opaque ancestor. Labels, check boxes, radio buttons,
  separators and buttons become transparent.
- **Local relayout.** A relayout measures the changed widget and its
  ancestors again. Measurements of unchanged subtrees are reused. The
  pass ends at the first ancestor whose measurement is unchanged. Only
  widgets whose rectangle changed are repainted. A resize of the window
  remains a full layout.
- **Scrolling by copy.** `widget_scroll_area` moves the pixels of the
  last paint within the window surface with the new `gfx_move_rect`
  and repaints the exposed rows only. The method relies on
  `gui_buffers_switch`, which gives the current buffer the latest
  contents (`buffers.c:64-81`). The moved area is still damage for
  X12, so the saving is on the client side. The copy falls back to a
  repaint for a popup in the window surface, a pending full repaint or
  a resize.
- **Proportions.** Corners have 6 px. Controls are `TM_CONTROL_H`
  (26 px) high. Rows of lists, trees and tables are the font height
  plus 6. Menu rows are `TM_CONTROL_H` high.
- **Theme.** `theme.h` gains `TM_ICON` (16), `TM_ROW_PAD` (6),
  `TM_INDENT` (16), `TC_BUTTON_CHECKED` and `theme_scale_px(t, px)`
  for the remaining literal sizes. The Lua binding extends its tables
  of colour and metric keys (`user/lua/lgui.c:1419-1441`).
- **Shapes.** Antialiased shapes come from `paint.c` on the coverage
  tables of `gui/pixel.h`. No widget and no program contains shape
  code of its own.
- **Strings.** Widget strings use `_()` of the libgui domain, as in
  `folderview.c:23-27`. The catalogues in `user/po/libgui/` receive the
  new strings.
- **Shared helpers replace their private copies:**
  - `scrollbar_paint_track` in `widget.h`. Private declarations are at
    `listview.c:32`, `models.c:10`, `editor.c:12` and
    `user/term/term.h:21`.
  - `GUI_DOUBLE_CLICK_MS` and `gui_click_count`. Copies are at
    `models.c:34`, `editor.c:1090-1092`, `user/term/term.c:636` and
    `client.c:378`.
  - The word boundary functions of `editor.c:374-405`, moved to
    `gui/utf8.h`.
  - An icon cache with the key name, size, scale and colour. Copies are
    in `user/panel/icons.c` and `user/apps/screenshot.c:725-733`.
  - Antialiased discs and rounded rectangles. A copy is at
    `screenshot.c:359-384`.
- **The panel.** The panel remains a layer surface drawn with the
  libgui painter on libwire (§2 of `desktop-panel.md`). It cannot host
  widgets: it has its own libwire connection, canvases and popups
  (`panel.h:58-75`), and a libgui window requires the libgui client.
  The panel therefore draws with the paint functions of the libgui
  classes. Its palette becomes two `struct theme` values, one for the
  bar and one for the menus, in place of the defines at
  `panel.h:37-55`.
- **ABI.** The libgui ABI rises from 2 to 3 in K2, because
  `struct widget`, `struct theme` and `struct gui_stats` grow
  (`lib/libgui/Makefile:10`).

## 3. Milestones

### K1. Scaled measurement and correctness defects (completed 2026-10-09)

Built:

- The scaled measurement of §2 replaces every unscaled call:
  `basic.c:29`, `controls.c:23`, `menu.c:41,43,180`,
  `textfield.c:98,149`, `window.c:208,311`, `dialog.c:54`,
  `folderview.c:1319`, the 12 calls in `editor.c` and the gutter width
  at `editor.c:487`, `user/term/term.c:190`, `user/apps/unicode.c:359`,
  and the dummy painter at scale 1 of the tabs
  (`containers.c:99-101`).
- One thumb geometry serves painting and hit testing of the scroll bar.
  The two geometries at `scroll.c:17-20` and `scroll.c:70-75` differ
  today. One helper for the scroll tracks inside the list view, the
  views and the editor replaces `listview.c:93-98`, `models.c:290-295`
  and `editor.c:1076-1086`. A press on the thumb of such a track drags
  the thumb.
- The colour button shows its pressed state (`color.c:240-259`). Menu
  navigation by keyboard skips separators and disabled items
  (`menu.c:157-158`). Menus underline the mnemonic and draw no `&`
  (`menu.c:93,204`). The caption code of `basic.c:7-49` becomes
  `painter_mnemonic_text`. The list view emits `activate` on a double
  click. Canvases are not focusable by default (`basic.c:333`).
- `icon_variant(icon, scale, color)` returns an icon from a cache with
  the key name, size, scale and colour. The cache grows and never
  returns NULL when full (`icons.c:43-44`).
- The colour dialog computes its field at device resolution
  (`color.c:9-11,57-58`).
- Accelerator labels (`menu.c:20-28`) and the progress text
  (`controls.c:343`) are translated.
- `gui.md:94-104`, `gui.md:249-261` and `gui.md:339` describe the
  current toolkit and the 30 px header. `desktop.md:171` describes the
  colour button. `CLAUDE.md` states the 30 px header.

Tests: `make check-libgui` gains `test_scale.c`. The test runs the
fake client at scale 2 with `third_party/dejavu/DejaVuSans.ttf`. It
requires the caret of a text field at the painted prefix width, a
click at a glyph boundary that places the cursor there, measured
widths of combo boxes, menus and tabs equal to their painted widths, a
tab hit at scale 2, one thumb geometry, the pressed colour button,
Down over a separator and a disabled item, no `&` in the pixels of a
menu item, `activate` after a double click, and two renditions of an
icon at scales 1 and 2. The new boot case `gui_widgets_scale2` runs
`widgettest scale` at `video=2048x1536@2`, clicks between two letters
of a text field and requires the cursor index in the log. The cases
`gui_widgets`, `gui_controls`, `gui_app`, `gui_files`, `gui_term`,
`gui_term_scale2`, `gui_calc`, `gui_unicode` and `gui_hexview` run
unchanged.

Document: `docs/design/widgets.md` (new), `docs/design/framework.md`,
`docs/design/gui.md`, `docs/design/desktop.md`, `docs/design/icons.md`.

### K2. Measurement and repaints without a change (completed 2026-10-09)

Built:

- The counters of §2 and the ABI raise.
- The `compbench` scenarios:
  - `wheel`: a table of 60 rows with 20 visible. 10 wheel steps up at
    the top, then 20 down. 14 of the 20 change the view and 6 are at
    the end.
  - `type`: 20 characters into a text field of 300 px.
  - `status`: a status bar label set 20 times.
  - `edit`: an editor with 4000 lines of C and the highlighter, 20
    characters at line 3500.
  - `scroll`: a scroll area of 200 labels, 20 steps.
  - `hover`: the pointer moved across 20 rows.
- The baseline is recorded in a commit before the fixes.
- The unconditional invalidations are removed: the wheel and paging at
  the ends (`listview.c:105-108`, `models.c:364-367`,
  `editor.c:1140-1147`), `scrollbar_set` (`scroll.c:141-148`),
  `widget_set_enabled` (`widget.c:159-163`), `select_flat` of an
  unchanged row (`models.c:102-113`), Home and End without a move in
  the text field (`textfield.c:196-209`), the slider release without a
  press (`controls.c:290-293`), and in the programs `hexview.c:222-233`,
  `unicode.c:296-305,491-496` and `view.c:126-131`.
- The local relayout of §2 replaces the full measure, layout and
  repaint of the window that each `widget_relayout` causes. The scroll
  area moves its content without a relayout of its ancestors
  (`scroll.c:169-175`).

Tests: `make check-libgui` gains `test_perf.c`. A wheel step and a page
click at each end add 0 paints and 0 damage. A label update in a
status bar paints at most 2 widgets, and its damage is the label
rectangle. A step of a scroll area adds 0 layouts. The boot cases
`gui_bench` and `gui_bench_hidpi` require `paints=14` for `wheel` and
`layouts=0` for `scroll` and print the other counters. `gui_app`,
`comp_bench` and `x12settings_perf` run unchanged.

Document: `docs/design/graphics-performance.md` (the counters and the
table before and after), `docs/design/framework.md` (layout).

### K3. Partial repaints, scrolling by copy and the editor (completed 2026-10-09)

Built:

- `widget_invalidate_rect`, a dirty rectangle per widget and the
  `transparent` bit. The text field invalidates from the first changed
  column to its end. A caret move invalidates the old and the new
  caret column.
- `widget_scroll_area` and `gfx_move_rect` in the list view, the tree
  view and table, the editor and the scroll area.
- The editor rebuilds the rows of the changed lines only. Today every
  edit rebuilds all rows (`editor.c:33-40,496-529`). A row index per
  line replaces the linear `row_of` (`editor.c:531-545`). The editor
  caches the highlighter state at the start of each line. The cache is
  valid up to the first edited line. A paint runs the highlighter from
  the first visible line (`editor.c:621-629`).
- The tree view and table map row ids to flat rows and record the
  expanded rows in a set. These replace the linear `flat_index_of` and
  `is_expanded` (`models.c:38-44,94-100`).
- The tabs and the menus compute their title widths once per layout
  instead of in each paint (`containers.c:54-67`, `menu.c:41-43,97`).
- A cache of shaped text is added if the `text_shapes` counter of K2
  shows repeated shaping of the same text per paint.

Tests: one keystroke in a field of 300 px damages at most the field
height times the width from the edit to the end. A paint after
scrolling by copy is equal in every pixel to a full repaint, at scale 1
and 2. A wheel step of 3 rows calls `cell` at most 3 times per column.
A paint at line 3500 runs the highlighter at most for the visible lines
plus one. One keystroke rebuilds the rows of one line. The boot cases
`gui_bench*` print the new numbers. `gui_editor`, `gui_code`,
`gui_files`, `gui_hexview` and `gui_unicode` run unchanged.

Document: `docs/design/graphics-performance.md`,
`docs/design/framework.md` (partial redraw).

### K4. Theme metrics and antialiased shapes (completed 2026-10-09)

Built:

- The radius of 6 px, `TM_ICON`, `TM_ROW_PAD`, `TM_INDENT` and
  `theme_scale_px`. Every literal size of the widgets uses them:
  `textfield.c:10`, `controls.c:7,236,332`, `containers.c:10,165`,
  `models.c:12,13,33`, `menu.c:7,8`, `color.c:9-11` and the minimum
  thumb at `scroll.c:19,74`.
- `paint.h` gains `painter_round_rect(p, x, y, w, h, r, fill, border)`
  with corners antialiased from `pixel_corner_table` and
  `pixel_round_rect_coverage`, `painter_disc`, `painter_ring`,
  `painter_stroke` for antialiased polylines, `painter_check`,
  `painter_chevron`, `painter_fill_alpha` and a rounded
  `painter_focus_ring`. `painter_rounded` is rebuilt on
  `painter_round_rect`.
- `screenshot.c:359-384` uses the new shapes. `csd.c` takes its colours
  from the theme in place of the defines at `csd.c:12-21`.

Tests: `make check-libgui` gains `test_paint.c`. The test checks the
corner coverage against the tables at scale 1 and 2, the symmetry of
chevrons and of the check mark, the end points of strokes, and that
`theme_scale_px` follows `ui_scale`. The new boot case
`gui_widgets_look` requires a partly covered pixel at a field corner,
with a colour between the field colour and the window colour, and
takes a screendump for a visual check. `gui_scale2`, the `screenshot`
cases and `make check-lua` run unchanged.

Document: `docs/design/widgets.md` (metrics and shapes),
`docs/design/framework.md` (theme and painter).

### K5. The new look of the widgets

Built:

- Check boxes with antialiased boxes and a painted check mark. Radio
  buttons as discs with a dot. Today the radio button is a square with
  a square dot (`basic.c:178-186`).
- Chevrons replace the line loops of the combo box, the spinner and
  the expanders (`controls.c:40-42,188-191`, `models.c:183-188`) and
  the ASCII sort mark (`models.c:142`).
- Hover on check boxes, radio buttons, combo boxes, spinner arrows,
  the slider thumb, tabs, menu bar titles, table headers and rows.
  Hover on rows uses partial invalidation. Pressed states on spinner
  arrows and table headers. A disabled look in every widget.
- Selected rows are rounded pills, as in `folderview.c:1150-1155`.
  Icons on selected rows take `TC_SELECTION_TEXT`.
- The editor selects with `TC_SELECTION` like the text field
  (`editor.c:656`, `textfield.c:128`). The current line uses
  `pixel_blend` (`editor.c:597-603`). The syntax colours move into one
  table in `editor_hl.c` (`editor.c:585-595`).
- The height of the progress bar follows the font
  (`controls.c:332`).
- A tooltip class in `window.c` with theme metrics replaces the label
  with the style flag (`basic.c:55-59`, `window.c:204-211`).

Tests: a hover move between rows damages two rows. A disabled widget
ignores input and paints `TC_TEXT_DISABLED`. The pixels of the check
mark, the radio dot and the chevrons are checked. The boot case
`gui_widgets_look` checks the new marks and takes a screendump.
`gui_bench*` require the damage of two rows per step in the `hover`
scenario. `gui_app` requires the hover colour `0x00d0d0d0` as before.
The `gui_*` cases that click list rows use the new row height.

Document: `docs/design/widgets.md`.

### K6. Missing functions

Built:

- The text field gains undo and redo with Ctrl+Z, Ctrl+Y and entries in
  the context menu. A masked field records no history. Ctrl+Left and
  Ctrl+Right, also with Shift, use the shared word functions. A double
  click selects a word, and a triple click selects all text.
  `textfield_set_placeholder` sets a placeholder.
- One caret blink per window for the focused text widget, with the
  period `GUI_CARET_BLINK_MS` (530). The blink invalidates only the
  caret. The blink ends 10 s after the last input, and the caret is
  then visible. The terminal uses the same period (`term.c:819`).
- The spinner accepts typed digits. Enter or a focus change commits
  the value, clamped to the range. `combobox_set_editable` adds the
  editable combo box of M22 (`m19-m26-display.md:264`).
- `label_set_wrap` on `painter_wrap` (`paint.c:234`), with a measure of
  the height for a given width, and `label_set_ellipsis`.
- `progress_set_pulse` for an indeterminate progress bar.
- `menuitem_set_check`, `menuitem_set_radio` and `menu_add_submenu`.
  The View menu of Files uses a check item for hidden files and radio
  items for the sort order (`user/files/files.c:475-480`).
- `imageview_new` and `imageview_set`: the image is centred, reduced to
  fit, never enlarged, scaled with `image_scale` and cached per size
  and scale. The class replaces `imageview` of the Lua binding
  (`user/lua/limage.c:292-330`). The Lua interface does not change.

Tests: one host test per function, and `make check-lua` with the image
view checks of `user/lua/tests/gui.lua`. The new boot case
`gui_widgets_text` (`widgettest text`) checks typing, undo, Ctrl+Left,
a word selection, typing in the spinner, a check menu item and a
submenu. `gui_files` and `gui_lua_bindings` run unchanged.

Document: `docs/design/widgets.md`, `docs/design/framework.md`,
`docs/design/lua.md` (the image view comes from libgui).

### K7. Classes of the greeter, the screen locker and askpass

Built:

- `spacer_new`.
- `backdrop_new(window, style)`: the desktop colour or wallpaper of
  `/etc/desktop.conf`, or a translucent dimming.
- `card_new(parent, width)`: rounded, padded and centred. In a
  translucent window the card sets the opaque region of the window to
  its rectangle without the corners.
- `account_new(parent, name, full_name, focusable)`: hover and
  selection pills and the signal `clicked`.
- `toolbar_set_center` for the clock of the top bar.
- These classes replace `user/greeter/screen.c:38-190`,
  `user/greeter/greeter.c:99-139` and `user/askpass/askpass.c:53-133`
  with the defines of `askpass.c:39-43`. The screen locker uses the
  classes through `screen.c`.

Tests: host tests check the opaque region of the card and the clicks
on account rows. The boot cases `gui_greeter`, `greeter_boot`,
`greeter_fallback`, `login_gui`, `gui_lock`, `gui_lock_crash`,
`lock_idle`, `gui_askpass` and `privilege` run unchanged.

Document: `docs/design/widgets.md`, `docs/design/users.md`,
`docs/design/lock.md`.

### K8. The panel on the shared paint functions

Built: these paint functions draw the libgui classes and the panel.

| Function | libgui class | Panel |
|---|---|---|
| `painter_button(p, x, y, w, h, state)` with the states hover, pressed, checked and disabled | button, menu bar titles, spinner arrows | buttons (`panel.c:203-261`), calendar navigation (`calendar.c:160-163`), notification actions and close buttons (`notify.c:154-181`) |
| `painter_field` | text field | launcher search field (`launcher.c:200-214`) |
| `painter_slider` | slider | mixer volume bar |
| `painter_meter` | progress | mixer level meter (`mixer.c:85-107`) |
| `painter_menu_row` | dropdown | launcher, power and input method rows (`power.c:45-67`, `imemenu.c:95`) |
| `painter_card` | card | notification card |
| `painter_switch` | the new class `switch_new` | do-not-disturb switch (`notify.c:358`) |

The panel palette becomes two themes. `panel_icon` uses the libgui
icon cache.

Tests: `make check-libgui` checks the state colours of
`painter_button` and `painter_switch`. The boot cases `comp_panel`,
`panel_calendar`, `panel_power`, `panel_desktop`, `panel_top`,
`gui_notify`, `gui_mixer`, `gpu_resize`, `gui_greeter` and `gui_lock`
run with unchanged colours at their sample points.

Document: `docs/design/shell.md` (the panel), `docs/design/widgets.md`.

## 4. Size

About 7600 changed lines with tests and documents: K1 1100, K2 900,
K3 1300, K4 800, K5 900, K6 1500, K7 500, K8 600.

## 5. Implementation notes

### General

- Each milestone starts with the shared function and then replaces the
  private copies that a search finds. `grep -rn "<old call>" lib user`
  before and after a change defines the finished state: the old
  pattern has zero hits.
- The call replacement of K1 and the metric replacement of K4 are
  repetitive. The grep check comes first. A Sonnet subagent receives
  the list of sites, and its result is verified against the check.
- `make check-libgui` runs in seconds and covers most of each
  milestone. Boot cases run once at the end through
  `make test-changed`.

### Measurement (K1)

- The fake client sets `w->scale = 1` in `alloc_surfaces`
  (`lib/libgui/tests/fake_client.c:47-55`). A global test scale read
  by `alloc_surfaces` and `gui_get_output` allows tests at scale 2.
  Each test resets the scale.
- The builtin font is exactly twice as wide at scale 2, so the
  measurement error does not appear with that font. `test_scale.c`
  loads `third_party/dejavu/DejaVuSans.ttf`.
- `painter_text_width` already measures at the scale of the painter.
  `widget_text_width` computes the same value from
  `gfx_text_width_font_scaled(font, text, n, scale)`, divided by the
  scale and rounded up. `csd.c:239` and `display.md:170-192` show the
  rule.
- `user/tests/widgettest.c:106-120` selects modes by `argv[1]`. The
  modes `scale` and `text` are added there.
- Before the default of the canvas changes, a search for `canvas_new`
  in `user/` lists the programs. Each program whose canvas connects
  `key` or `text` sets `focusable = 1`.

### Counters (K2)

- `struct gui_stats` is in `lib/libgui/include/gui/client.h:194-202`.
  The new fields go at its end. `ABI := 2` in `lib/libgui/Makefile:10`
  rises in the same commit.
- `widget_paints` is counted in `paint_tree` of `window.c`, `layouts`
  in `widget_measure` and `layout_tree`, and `text_shapes` in the
  shaping entry of `font.c`.
- The scenarios go into `user/tests/compbench.c`.
  `tools/benchtable.py` already formats its output.
- The baseline numbers are committed before the fixes. The table in
  `graphics-performance.md` then has a measured column before the
  change.

### Local relayout (K2)

- The pass compares the new size hints with the previous hints of each
  ancestor. The pass ends at the first ancestor with equal hints and
  lays out the subtree of that ancestor.
- Only children whose rectangle changed are repainted.
- A resize of the window remains a full layout. `window_paint`
  (`window.c:82-89`) takes the full path only when the window itself
  has `needs_layout`.

### Scrolling by copy (K3)

- `gfx.h` has `gfx_blit` and `gfx_copy_rect` (`gfx.h:94-96`), but no
  move within one surface. `gfx_move_rect` calls `memmove` per row,
  from the top or from the bottom according to the direction.
- The copy works in device pixels. The device offset is the scroll
  offset in logical rows times the scale.
- The test that compares a copy scroll with a full repaint finds rows
  that are off by one at scale 2.

### Editor (K3)

- The state at a line start is a small integer of the highlighter in
  `editor_hl.c`. An edit at line N invalidates the cached states from
  line N on.
- The `edit` scenario measures the editor before and after the change.
  A file of 4000 lines shows the linear costs.

### Look (K4, K5)

- Coverage comes from `pixel_corner_table` and
  `pixel_round_rect_coverage` in `gui/pixel.h`. `csd.c:146-180` and
  `csd.c:331-338` cache the coverage per scale. The new code uses the
  same method without copying the code.
- Shapes are drawn in device pixels inside `paint.c`, so that edges at
  scale 2 are sharp.
- After the radius change to 6 px, the cases with pixel sample points
  run: `gui_app`, `gui_scale2`, `x12settings_*` and `gui_greeter`. No
  sample point may lie on a corner.

### Panel (K8)

- The panel colours at the sample points of `comp_panel` and
  `panel_top` are compared before and after the change. The palette
  moves into themes, and the values do not change.
