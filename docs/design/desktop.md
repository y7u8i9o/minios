# Desktop, wallpaper, MIME types and settings

## Desktop client

`user/desktop/desktop.c` is a framework application whose only window
is a layer surface on the background layer (layer 0), anchored to all
four edges with a size of 0x0. The compositor answers a layer
configure with a zero dimension by the area left free by exclusive
zones (`layer_configure` in `user/compositor/shell.c`), and
reconfigures such layers whenever a layer with an exclusive zone maps,
so the desktop always covers the screen minus the panel. The scene
sorts layers 0 and 1 below every toplevel.

libgui gained `gui_create_layer_window` (`lib/libgui/src/client.c`) and
`app_layer_window` (`lib/libgui/src/app.c`). A layer window has no
decorations, acknowledges configure events and resizes its surface
when the configured size changes, and receives key events when created
with keyboard interactivity.

The desktop widget paints the wallpaper, then one cell of 90x84 pixels
per entry of `desktop` in the user's home (`users.md`), filled column by column. Directories are
listed first, then files by name. Each entry shows its icon at twice
the 16x16 size and its name, with the `.app` extension removed for
launcher files. A left click selects an entry and a second click
within 500 ms opens it. Enter opens the selection, F5 refreshes the
listing, Delete asks for confirmation and removes the entry. A right
click opens a context menu (`popupmenu_new` and `menu_popup` in
`lib/libgui/src/widgets/menu.c`, the menu bar's dropdown without a bar):

- On an entry: Open, Open with (the application chooser of
  `framework.md`), Rename, Delete. A command that fails to start shows
  a message with the error.
- On the desktop: New folder, New text file, Refresh, Change wallpaper
  (starts `settings appearance`), Settings.

An icon pressed and moved by more than four pixels is dragged as
`text/uri-list` and `text/plain` with its icon and label. Files dropped on
a folder icon go into that folder and files dropped elsewhere on the
desktop into `~/desktop`, moved when they are on the same file system and
copied otherwise (Ctrl copies, Shift moves). The folder icon under the
drag is drawn with an accent border, and failures are shown in a dialog
(`dnd.md`).

Every second the desktop re-reads its folder and compares the
configuration file (`$HOME/.config/desktop.conf` when it exists, the
shipped `/etc/desktop.conf` otherwise; `storage.md`) with the last
contents. A changed file is applied:
the wallpaper is reloaded and rescaled, and `desktop_color`,
`repeat_rate` and `repeat_delay` are sent to the compositor through
the `settings` protocol interface. Log lines: `desktop: started with N
entries`, `desktop: wallpaper PATH mode M`, `desktop: config applied`,
`desktop: open PATH`, `desktop: menu NAME|desktop`, `desktop: drag PATH`,
`desktop: drop moved|copied N into DIR`.

## Wallpaper

Wallpapers are PNG files in `/usr/share/wallpapers`. `default.png` and
`dusk.png` are gradients of 2560x1600 pixels, the largest mode of the
GPU, which `tools/genwallpapers.py` draws. The gradients are dithered
with a 4x4 ordered pattern, and the band of `default.png` has smoothed
edges. Until B1 of `docs/plan/desktop-panel.md` the two files had 320x240
pixels, and the desktop enlarged them eight times on a screen of
2560x1600 pixels.

`wallpaper_render` of libgui (`gui/wallpaper.h`) draws the background of
an area: the desktop colour with the image over it in one of four modes.
`fill` scales the image to cover the area, retaining the aspect ratio,
and cuts the longer side. `center` draws the image in the middle and
`tile` repeats it from the top left corner, both with one image pixel
per logical pixel. `stretch` scales both dimensions independently. The
result has the device size of the area and the scale of the output, and
`painter_image` copies it pixel for pixel. The image is scaled with
`image_scale` (`images.md`), so the wallpaper has the full resolution of
the device and filtered edges. The desktop renders the background once
per change of the size, the scale, the image, the mode or the colour. The
greeter renders the wallpaper of the system settings in the mode `fill`
in the same way. Before B1 the desktop scaled the image to the logical
size of its window with the nearest source pixel, and the painter then
doubled every pixel at scale 2.

`wallpaper_hidpi` boots `video=2560x1600@2`. A wallpaper of 2560x1600
pixels in vertical stripes of one pixel must appear pixel for pixel, and
a wallpaper of 2x2 pixels stretched to the screen must show the
interpolated colour between its red and its green pixel. The QMP script
then takes a screendump of the default wallpaper for inspection.

## Configuration file

`/etc/desktop.conf` contains `key=value` lines:

- `wallpaper`: path of the PNG, empty for none, which is the default: the
  desktop is the solid `desktop_color` until a wallpaper is chosen.
- `wallpaper_mode`: `fill`, `center`, `tile` or `stretch`.
- `desktop_color`: `0xRRGGBB`, the colour under and around the wallpaper.
- `pointer_speed` (-100..100) and `pointer_accel` (flat or adaptive):
  the compositor's pointer acceleration (`input.md`), forwarded like the
  key repeat; the Mouse page of the settings program edits them.
- `repeat_rate` and `repeat_delay`: keyboard repeat, forwarded to the
  compositor.
- `display_mode`: `WxH` or `WxH@S`, forwarded as the compositor's packed
  `display_mode` setting; absent means the boot mode (M32).
- `display_follow`: 1 or 0, forwarded to the compositor. With 1 the mode
  follows the window size of the host display (`display.md`, V3 of the
  0.6.0 release). The default is 1.
- `panel_position`: `bottom` or `top`, the edge of the screen of the
  panel (B6 of `docs/plan/desktop-panel.md`). The panel reads it itself
  with `conf_lookup` once a second. The default is `bottom`.

The keys `lang` and `formats` name locales such as `fr_FR.UTF-8`
(`locale.md`). `startgui` exports `lang` as `LANG` and `formats` as
`LC_NUMERIC`, `LC_TIME` and `LC_MONETARY` before it starts the session,
through `conf_export_locale`. Without `formats` the three variables are
removed and the categories follow `LANG`.
`/etc/profile` exports them in the same way for the shells of the console
and of the terminal. An empty value exports nothing, and programs then use
the C locale. `term_font_px` is the font size of the terminal.

## MIME types

`lib/libgui/src/mime.c` (`gui/mime.h`) reads two tables on first use:

- `/etc/mime.types`: `type ext ext ...` lines. `mime_type(path, is_dir)`
  matches the extension case-insensitively and returns
  `application/octet-stream` when nothing matches, `inode/directory`
  for directories.
- `/etc/mime.apps`, or the user's `~/.config/mime.apps` when it exists
  (`users.md`): `type command` lines. A command is a program followed
  by arguments separated by spaces. `mime_handler(type)` tries the
  exact type, then `type/*`, then `*`.

`mime_load` with a NULL path reloads the table from the path of an
earlier call, else from the default path. `mime_open(path)` starts the
handler through `mime_run` and returns 1 or a negative errno.
`mime_run(command, path)` splits the command at spaces and appends the
path to the arguments. The tables of installed packages in
`/var/lib/pkg` (`packages.md`) are read after the system tables.
`mime_handler` ignores a package entry for a type with an entry in the
system or user table. `mime_handlers(type)` lists the commands of the
type in the order of `mime_handler`, the overridden package entries
included. The application chooser uses that list for its recommended
applications. `mime_save` writes the system entries only. A launcher
file (`application/x-launcher`, extension `.app`) is opened by running
the command in its `exec=` line instead. `mime_icon(type)` returns the
icon in `/usr/share/icons`. `mime_set_handler` and `mime_save` edit the
handler table. The Files application (`files.md`) opens files through
`mime_open`.

Programs are started by `mime_spawn`. The function forks twice and
reaps the intermediate child at once. The program becomes a child of
init and is reaped there when it exits. A launcher without a wait loop
(Files, the desktop, Settings) therefore never accumulates zombies. The
`gui_desktop` case checks that the clock opened by a double click is
not the desktop's zombie. A pipe reports a failed exec to the caller.
The write end of the pipe has `FD_CLOEXEC`. The grandchild writes the
errno of a failed `execvp` to the pipe. A successful exec closes the
write end, and the caller reads end of file. `mime_spawn` returns 0 or
the negative errno.

## Settings application

`user/settings/` (`/bin/settings`) is the user's settings window: a
category list on the left and one page on the right. Every change is
written to `/etc/desktop.conf` at once (there is no Apply button); the
desktop client reads the file within a second and pushes the values X12
owns through the settings protocol. `settings set KEY VALUE` changes one
entry without a window, `settings PAGE` opens on a page (`appearance`,
`display`, `keyboard`, `region`, `mouse`, `sound`, `time`, `filetypes`,
`launcher`, `system`).

- Appearance: wallpaper (the files of `/usr/share/wallpapers` or none),
  placement, desktop colour sliders with a preview, the interface font
  (`ui_font`: DejaVu Sans, Noto Sans, Latin Modern Roman or the builtin
  bitmap font), its size (`ui_font_px`) and the interface scale
  (`ui_scale`, 100, 125 or 150 percent). libgui reads the three `ui_`
  keys in `theme_init_default` (`theme_read_conf`), so they apply to
  programs started afterwards. The last row is the panel position
  (`panel_position`, Bottom of the screen or Top of the screen), which
  the running panel applies within a second.
- Display: the current mode, resolution and pixel density
  (`display_mode`), the check box for `display_follow`, the frame
  interval (`frame_ms`) and the decoration
  side (`decorations`, `client` or `server`); the desktop pushes the last
  two to X12 like the colour and the key repeat.
- Keyboard: the layout (`keymap`, the `.mkm` files of
  `/usr/share/keymaps`; the desktop sends `keymap_reload` and X12 reloads
  the file, which reaches clients bound afterwards) and the repeat rate
  and delay.
- Sound: the master volume and the streams of the audio server with a
  volume slider for the selected stream, through the mixer interface of
  libaudio; without `audiod` the page shows "No audio server".
- Date and time: the local time with the zone abbreviation, fields for a
  new local date and time, and Set clock (`settimeofday`).
- File types: the handler table with a program field, Set and Add type;
  writes the user's `~/.config/mime.apps` immediately.
- Launcher: the entries of the user's `~/.config/launcher`, or of
  `/etc/launcher` before the first change, which the page saves to the
  user's file, with fields for the title and
  program, Save, New, Remove, Move up and Move down; the panel reads the
  file when it starts.
- System: kernel name and release, processors, display, uptime, memory
  and swap in use, and buttons that start the system monitor, the kernel
  log and the X12 tool.

The Region and language page (`region.c`, L7 of `docs/plan/locale.md`)
lists the locales of `/usr/share/i18n/locales` by the language and
territory names of their files, for example Français (France). The
Language list writes `lang`, and the Formats list writes `formats`, whose
first entry, Same as the language, writes an empty value. The panel reads
the file every second and the desktop when it changes. Both then call
`conf_export_locale` of libc, which sets the variables of the setting in
their environment, and `setlocale`. The panel draws its labels and the
next launcher menu in the new language, and the desktop builds its context
menus again. Programs started from the panel or the desktop, and every
program that `mime_spawn` starts, inherit the new variables. A program
that was already running remains in its language, and a new session is not
needed. The Time zone list shows the zones of
`/usr/share/zoneinfo/zones.tab` and replaces `/etc/localtime` with a
symbolic link to the selected zone file. libc reads the zone again when the
link changes (`time.md`), and the panel clock follows at once. The page
also shows the keyboard layout, in a combo box that the Keyboard page
shares, a line with the current time and a number in the selected formats,
and the input method section that `ime.md` describes (`ime_engines`,
`ime_shift_toggle`, `ime_ctrl_space`, `ime_page_size`, `ime_orientation`).

`x12settings` changes the running server (see `tools.md`).

## Session

The panel also carries the audio applet described in `docs/design/audio.md`:
a speaker button left of the clock opens a popup with the master volume and
the streams of the audio server.

`startgui` exports the language and the formats of the configuration
file, starts X12, the panel, the desktop and the requested program, and
stops them on logout. The audio server is the `audio` service of
init (`init.md`), started at boot when `/dev/pcm0` exists and living
across sessions. A desktop or panel that ends abnormally is
restarted up to three times. The `gui_desktop` boot test covers the
wallpaper, opening a launcher by double click, both context menus, a
configuration change through `settings set` and the settings window. The
`gui_region` boot test selects French and Asia/Tokyo on the Region and
language page with the keyboard, then checks `lang` in the configuration
file, the zone abbreviation that `date` prints and the `LANG` that
`/etc/profile` exports. The running panel and desktop must report the new
language. The launcher must draw its icons, and sysmon started from it
must show its French title. The desktop must survive its context menu
built again.
The host test `lib/libgui/tests/test_mime.c` covers the tables.
