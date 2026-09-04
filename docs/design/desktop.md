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

libgui gained `gui_create_layer_window` (`libgui/src/client.c`) and
`app_layer_window` (`libgui/src/app.c`). A layer window has no
decorations, acknowledges configure events and resizes its surface
when the configured size changes, and receives key events when created
with keyboard interactivity.

The desktop widget paints the wallpaper, then one cell of 90x84 pixels
per entry of `/home/desktop`, filled column by column. Directories are
listed first, then files by name. Each entry shows its icon at twice
the 16x16 size and its name, with the `.app` extension removed for
launcher files. A left click selects an entry and a second click
within 500 ms opens it. Enter opens the selection, F5 refreshes the
listing, Delete asks for confirmation and removes the entry. A right
click opens a context menu (`popupmenu_new` and `menu_popup` in
`libgui/src/widgets/menu.c`, the menu bar's dropdown without a bar):

- On an entry: Open, Open with (program prompt), Rename, Delete.
- On the desktop: New folder, New text file, Refresh, Change wallpaper
  (starts `settings appearance`), Settings.

Every second the desktop re-reads `/home/desktop` and compares
`/etc/desktop.conf` with the last contents. A changed file is applied:
the wallpaper is reloaded and rescaled, and `desktop_color`,
`repeat_rate` and `repeat_delay` are sent to the compositor through
the `settings` protocol interface. Log lines: `desktop: started with N
entries`, `desktop: wallpaper PATH mode M`, `desktop: config applied`,
`desktop: open PATH`, `desktop: menu NAME|desktop`.

## Wallpaper

Wallpapers are PNG files in `/usr/share/wallpapers` (`default.png`,
`dusk.png`, generated 320x240 gradients). The desktop scales the image
to its window once per size change with nearest neighbour sampling
and 16.16 fixed point ratios. Modes: `fill` scales to cover the window
keeping the aspect ratio and crops the excess, `center` draws the
image unscaled in the middle over the desktop colour, `tile` repeats
it, `stretch` scales both dimensions independently.

## Configuration file

`/etc/desktop.conf` holds `key=value` lines:

- `wallpaper`: path of the PNG, empty for none, which is the default: the
  desktop is the solid `desktop_color` until a wallpaper is chosen.
- `wallpaper_mode`: `fill`, `center`, `tile` or `stretch`.
- `desktop_color`: `0xRRGGBB`, the colour under and around the wallpaper.
- `repeat_rate` and `repeat_delay`: keyboard repeat, forwarded to the
  compositor.
- `display_mode`: `WxH` or `WxH@S`, forwarded as the compositor's packed
  `display_mode` setting; absent means the boot mode (M32).

## MIME types

`libgui/src/mime.c` (`gui/mime.h`) reads two tables on first use:

- `/etc/mime.types`: `type ext ext ...` lines. `mime_type(path, is_dir)`
  matches the extension case-insensitively and returns
  `application/octet-stream` when nothing matches, `inode/directory`
  for directories.
- `/etc/mime.apps`: `type program` lines. `mime_handler(type)` tries the
  exact type, then `type/*`, then `*`.

`mime_open(path)` starts the handler with the path as its argument and
returns the child's pid. A launcher file (`application/x-launcher`,
extension `.app`) is opened by running the command in its `exec=`
line instead. `mime_icon(type)` names the icon in `/usr/share/icons`.
`mime_set_handler` and `mime_save` edit the handler table. The Files
application opens files through `mime_open`.

## Settings application

`user/settings/` (`/bin/settings`) is the user's settings window: a
category list on the left and one page on the right. Every change is
written to `/etc/desktop.conf` at once (there is no Apply button); the
desktop client reads the file within a second and pushes the values X12
owns through the settings protocol. `settings set KEY VALUE` changes one
entry without a window, `settings PAGE` opens on a page (`appearance`,
`display`, `keyboard`, `sound`, `time`, `filetypes`, `launcher`,
`system`).

- Appearance: wallpaper (the files of `/usr/share/wallpapers` or none),
  placement, desktop colour sliders with a preview, the interface font
  (`ui_font`: DejaVu Sans, Noto Sans, Latin Modern Roman or the builtin
  bitmap font), its size (`ui_font_px`) and the interface scale
  (`ui_scale`, 100, 125 or 150 percent). libgui reads the three `ui_`
  keys in `theme_init_default` (`theme_read_conf`), so they apply to
  programs started afterwards.
- Display: the current mode, resolution and pixel density
  (`display_mode`), the frame interval (`frame_ms`) and the decoration
  side (`decorations`, `client` or `server`); the desktop pushes the last
  two to X12 like the colour and the key repeat.
- Keyboard: the layout (`keymap`, the `.mkm` files of
  `/usr/share/keymaps`; the desktop sends `keymap_reload` and X12 reloads
  the file, which reaches clients bound afterwards) and the repeat rate
  and delay.
- Sound: the master volume and the streams of the audio server with a
  volume slider for the selected stream, through the mixer interface of
  libaudio; without `audiod` the page shows "No audio server".
- Date and time: the clock in UTC, fields for a new date and time, and
  Set clock (`settimeofday`).
- File types: the handler table with a program field, Set and Add type;
  writes `/etc/mime.apps` immediately.
- Launcher: the entries of `/etc/launcher` with fields for the title and
  program, Save, New, Remove, Move up and Move down; the panel reads the
  file when it starts.
- System: kernel name and release, processors, display, uptime, memory
  and swap in use, and buttons that start the system monitor, the kernel
  log and the X12 tool.

`x12settings` changes the running server (see `tools.md`).

## Session

The panel also carries the audio applet described in `docs/design/audio.md`:
a speaker button left of the clock opens a popup with the master volume and
the streams of the audio server.

`startgui` starts `audiod` when a PCM device is present, then X12, the panel,
the desktop and the requested program. It stops the audio server with the
desktop session. A desktop or panel that ends abnormally is
restarted up to three times. The `gui_desktop` boot test covers the
wallpaper, opening a launcher by double click, both context menus, a
configuration change through `settings set` and the settings window.
The host test `libgui/tests/test_mime.c` covers the tables.
