# The desktop panel and the wallpaper

This plan renews the panel of the graphical session and the drawing of the
wallpaper. The panel is the bar of `user/panel/` with the launcher menu,
one button per window, the label of the input method, the mixer and the
clock. Its look and its functions date from M19 to M26. Each milestone
below ends with boot tests in `tests/cases/` and is marked completed here
when they pass.

## 1. Scope

The owner chose the following scope on 2026-10-06.

- The panel receives a new look and new functions. It remains a layer
  surface drawn with the libgui painter on libwire.
- Each window button shows the icon of the application and the title of
  the window. The buttons become narrower when many windows are open.
- New functions: a calendar under the clock, a power menu with Log out,
  Restart and Shut down, and a button that shows the desktop.
- The panel is at the bottom or at the top of the screen. Settings offers
  the choice, and the bottom is the default.
- The wallpaper is drawn at the full device resolution and scaled with
  filtering.

## 2. Fixed decisions

- The panel retains the minios proportions and palette: a bar of 28 px,
  buttons of 20 px with corners of 6 px, the background `0x0023272c` and
  the accent `0x005b9cf5`. Buttons show a highlight under the pointer.
- The icon of a window comes from its app_id. libgui sets the app_id of
  every window to the name of the program, the last part of `argv[0]` as
  libc reports it. The icon of a program is `/usr/share/icons/app-NAME.svg`
  or `app-default.svg`. One libgui function implements this rule and
  replaces the two copies in the launcher of the panel and in the Open
  with chooser.
- The clock shows the time without seconds and the date in the format of
  `LC_TIME`. A click opens a calendar of the month with today marked and
  buttons for the previous and the next month.
- The power menu replaces the entry Log out of the launcher. Restart and
  Shut down send their request to init, as the buttons of the greeter do.
  init accepts them from the user of the graphical session (`users.md`).
  One libc function sends a request to init and replaces the copies in
  the greeter and in `initctl`.
- Show desktop is a function of the panel alone: the panel minimizes
  every window and records the windows that were visible. A second click
  activates them again, the formerly active window last. A window that
  opens in between ends the recorded state. The window protocol does not
  change.
- The position is the key `panel_position` (`bottom` or `top`) of
  `desktop.conf`. The panel reads the file every second, as it already
  does for the language, and moves itself after a change. Its menus open
  upwards from a bottom panel and downwards from a top panel.
- The wallpaper is scaled with `image_scale` of libgui at the device
  resolution of the output and drawn without enlargement. `image_scale`
  averages the covered source pixels when it reduces an image, as it does
  now, and interpolates bilinearly when it enlarges one. The desktop and
  the greeter use it in place of their own scaling.
- The boot tests take the geometry of the panel from one header,
  `kernel/tests/gui_helpers.h`, in place of the copies of `PANEL_H`,
  `CLOCK_W`, `MIXER_W` and `INPUT_W` in several test files.

## 3. Milestones

### B1. The wallpaper at device resolution with filtering (completed 2026-10-06)

`image_scale` gains bilinear enlargement. The desktop scales the
wallpaper to the device size of its window with `image_scale` and marks
the result with the scale of the output, so that the painter copies it
pixel for pixel. The modes fill, center, tile and stretch remain. The
greeter draws its wallpaper in the same way.

Tests: `make check-libgui` checks the enlargement of a two pixel image
(the pixels between the two colours lie between them) and the reduction
(a checkerboard of black and white reduced by two becomes grey).
`wallpaper_hidpi` boots `video=2560x1600@2` with a wallpaper of
2560x1600 device pixels in vertical stripes of one pixel and requires two
neighbouring device pixels of the screen to differ. A wallpaper of 2x2
pixels in the mode stretch must show intermediate colours in the middle
of the screen.

The two wallpapers of the system had 320x240 pixels. `tools/genwallpapers.py`
draws the same designs at 2560x1600 pixels with dithered gradients and
smoothed edges, because no filter adds the detail that such a small file
lacks.

Document: `docs/design/desktop.md`, `docs/design/images.md`.

### B2. The new look and the window buttons with icons (completed 2026-10-06)

libgui sets the app_id of every window. The shared icon function of
libgui replaces the copies in the launcher and in the Open with chooser.
The panel records the app_id of each window and draws its buttons with
icon and title. A button is 160 px wide and becomes narrower down to 40
px, at which width it shows the icon alone. The Menu button receives an
icon, every button receives the highlight under the pointer, and the
clock shows the date and the time without seconds.

Tests: `comp_panel` requires the icon of the test window on its button,
the highlight under the pointer, and the narrower buttons with ten
windows. The cases that click the panel take its geometry from
`gui_helpers.h`.

The icons `menu`, `calendar`, `power-off`, `restart` and `show-desktop`
were added to the table of `tools/fetch_icons.sh` for B2 to B5. The clock
became 128 pixels wide, the width of "Mon 5 Oct 20:38" with its margins.
`comp_panel` takes a screendump of the bar for inspection.

Document: `docs/design/shell.md` (the panel), `docs/design/gui.md`.

### B3. The calendar under the clock (completed 2026-10-06)

A click on the clock opens a popup with the month, the days of the week
in the order of the locale, the days of the month with today marked, and
buttons for the previous and the next month. A second click or Escape
closes it.

Test: `panel_calendar` opens the calendar, requires the current month
and the marked day, moves one month forward and back, and closes it.

The libc lacked the first day of the week. `nl_langinfo(_NL_FIRST_WEEKDAY)`
and the key `first_weekday` of the locale files provide it
(`docs/design/locale.md`), and `localetest` checks it. The cells are 36
pixels wide, the width of "Wed" with margins.

Document: `docs/design/shell.md` (the panel), `docs/design/locale.md`.

### B4. The power menu (completed 2026-10-06)

A button at the right end of the panel opens a menu with Log out, Restart
and Shut down. Log out ends the session as the launcher entry did, and
the entry leaves the launcher. Restart and Shut down send `reboot` and
`poweroff` to init through the new libc function.

Tests: `panel_power` logs in through the greeter, chooses Shut down and
requires the power off of the machine with exit status 0. `gui_greeter`
logs out through the power menu.

`panel_power` starts `/bin/init` itself, because `ktest_start_init`
replaces the greeter with the console login. The login steps of the
greeter and the choice of a row of the power menu became helpers of
`gui_helpers.h`. The panel catalogues took the translations of the three
rows from the catalogues of the greeter and the launcher.

Document: `docs/design/shell.md` (the panel), `docs/design/init.md`,
`docs/design/libc.md`.

### B5. Show desktop (completed 2026-10-06)

A narrow button at the edge of the panel minimizes every window and
restores them on a second click.

Test: `panel_desktop` opens three windows, requires the desktop colour
where they were after the first click, and the windows and the active
window after the second click. A window opened in between ends the
recorded state.

The test found a defect of X12: a toplevel accepted only the
acknowledgement of its latest configure, and the activation of several
windows in turn disconnected a client. A toplevel now records its last
four configures, as a layer surface does (`docs/design/shell.md`).

Document: `docs/design/shell.md` (the panel and the toplevel role).

### B6. The position at the top or at the bottom (completed 2026-10-06)

The panel reads `panel_position` and anchors itself at the top or at the
bottom with its exclusive zone. Its menus open in the direction of the
screen. Settings offers the choice on the page Appearance.

Test: `panel_top` sets `panel_position=top`, requires the panel colour in
the top row and the desktop colour in the bottom row, a maximized window
below the panel, and the launcher menu below its button.

X12 did not lay the screen out again when a mapped layer changed its
anchor or its exclusive zone. It now does so on the commit after the
change. The libc gained `conf_lookup` for the value of one key, which
replaced three private readers. The catalogues of Settings received the
translations of the new strings and of the string of V3 of the 0.6.0
release, which lacked them.

Document: `docs/design/shell.md` (the panel), `docs/design/desktop.md`,
`docs/design/compositor.md`, `docs/design/libc.md`.

## 4. Size

The work changes about 2500 to 3500 lines: the panel, libgui, libc, the
desktop, the greeter, Settings, six new or changed boot cases and the
documents.
