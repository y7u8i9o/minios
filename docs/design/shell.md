# X12 shell, seat, data device and panel

M25 gives surfaces roles and delivers input and data transfers; X12 retains
the protocol interface names stable for clients.
Protocol definitions: `protocol/shell.xml`, `seat.xml`, `data.xml`;
compositor modules: `user/compositor/shell.c`, `decor.c`, `seat.c`,
`data.c`. The panel consists of `user/panel/panel.c`, `launcher.c` and
`mixer.c`.

## Roles (`shell.c`)

- `shell.get_toplevel(surface)`: a top level window. The compositor
  sends `configure(serial, width, height, states)` with 0x0 at creation
  (the client chooses its size), and again with a size on maximize,
  restore, an interactive resize, and with the activated state on
  focus changes; the client answers `ack_configure(serial)` and commits
  a buffer of that size. The toplevel records its last four configures,
  as a layer surface does. The client may acknowledge any of them, and
  the acknowledged one supersedes the older ones. A buffer of the size of
  an acknowledged older configure is accepted while the newer one is
  pending. Until B5 of `docs/plan/desktop-panel.md` only the latest serial
  was valid. Two configures in quick succession, such as the deactivation
  and the activation of a window while several windows are activated in
  turn, then crossed the acknowledgement of the first, and X12
  disconnected the client with "invalid configure serial". The first
  commit with a buffer places the window in a cascade inside the desktop
  area and activates it.
  Requests: `set_title`, `set_app_id`, `set_min_size`, `set_max_size`,
  `move(seat, serial)` and `resize(seat, serial, edges)` (accepted with
  the serial of the last input event, they start the same drags as the
  decorations), `set_maximized`, `unset_maximized`, `set_minimized`.
  `close` asks the client to close.
- `shell.get_popup(surface, parent, positioner)`: placed relative to
  the parent by the positioner (size, anchor rectangle, anchor and
  gravity points, offset) and constrained with flip, slide and resize;
  `configure(serial, x, y, w, h)` reports the final position. The
  client acknowledges that serial before attaching the first buffer.
  `grab(seat, serial)` is accepted only for the initiating pointer
  press and makes an outside click or Escape dismiss it with `done`.
- `shell.get_layer_surface(surface, layer, namespace)`: anchored to
  screen edges with `set_anchor`, a size, an exclusive zone that
  shrinks the desktop area for toplevels, and keyboard interactivity;
  `configure(serial, w, h)` follows `set_size`. `set_margin(top, right,
  bottom, left)` measures the anchored edges from the desktop area
  instead of the screen (`compositor.md`). A surface without an anchor
  on an axis is centred on that axis.
- `shell.get_decoration(toplevel)` with `set_mode` (1 server, 2
  client): the compositor draws title bars, boxes and the resize grip
  only in server mode.
- `toplevel_manager` (a global): every bound manager receives a
  `toplevel_handle` per toplevel with `title`, `app_id` and `state`
  events (maximized 1, activated 2, minimized 3) and `closed`; handles
  accept `activate`, `minimize` and `close`.

## Decorations

Decorations follow the GTK 4 model: the toolkit draws its own header
bar, outline, rounded corners and shadow (client side decorations,
`lib/libgui/src/csd.c`, see `gui.md`), and the server only composites,
places the window and drives its move and resize drags. A client asks
for that through `shell.get_decoration` and `decoration.set_mode(2)`;
the server grants the client's choice and answers with `mode`. Clients
that never ask (comptest) get the mode of the `decorations` setting,
server side by default.

`toplevel.set_window_geometry(x, y, w, h)` names the visible window
inside the surface, without the client's shadow margins. The server
retains it in `struct toplevel.geo` and works on the visible frame,
`toplevel_frame` (the server decorations, the geometry, or the surface):
configure sizes are geometry sizes (`toplevel_configure_size`), the
cascade puts the frame of the n-th new window at (40, 24) plus n times
(30, 30) of the desktop area, so that the contents under a 36 pixel
header bar start at y 60; a modal child is centred on its parent's
frame; `clamp_toplevel` retains 40 pixels of the frame and its top row on
screen; a maximized window's geometry is the desktop area. The commit
check compares the acknowledged configure with the geometry when one is
set. The `mapped at` log line reports the frame.

Server side decorations (`decor.c`) remain for clients without their own,
drawn into the scene around the surface at the screen scale, antialiased
where shapes are curved:

- a 28 pixel title bar with rounded top corners (`RADIUS` 8), light
  (`0xe9ecf0` active, `0xf4f5f7` inactive) under a one pixel border and
  above a one pixel separator; the title in the interface font (DejaVu
  Sans 13 px, the builtin font when the file is missing), centred when
  it fits left of the buttons;
- three round buttons at the right, from the right: close (red with a
  white cross), maximize and minimize; inactive windows draw them grey;
- a soft drop shadow of `SHADOW` pixels reach, shifted `SHADOW_DY` down,
  fading quadratically, stronger for the active window, computed only
  for the band of the damage outside `decor_opaque` (the frame minus its
  rounded top rows, also the opaque cover for occlusion) from a table by
  squared distance built once per scale; the antialiased corner math runs
  only inside the two corner squares;
- maximized windows have square corners and no shadow.

Presses on server decorations never reach the client: the title bar
moves, the buttons act, the 1 px border, an invisible `RESIZE_MARGIN`
(6 px) around the frame and the bottom right grip of the contents resize
on release with a configure of the new size. Margins within 24 px of a
corner take both edges. A square at each corner reaches 12 px outside the
frame, and at the top corners 8 px inside over the rounded corner, and
takes both edges as well. The client decorations of libgui use the same
function, `gui_resize_edges` (`lib/libgui/src/gfx.c`), with their own
sizes (`gui.md`). Alt with the
left button moves any toplevel from anywhere, whichever side decorates.

A move or resize the client requested (`toplevel.move`, `toplevel.resize`
with the press serial) is driven by the same drag code, but the release
that ends it is still delivered to the client, so its button state remains
consistent (`decor_release` returns 2 for such drags). A move or resize
request is honoured only while a pointer button is pressed: one that
arrives after the release, for example from a client that answered late,
is ignored instead of starting a drag with no button pressed. Popup grabs
continue accepting the press serial after the release, as the panel needs.

## Seat (`seat.c`)

One seat with pointer and keyboard capabilities. The pointer focus is
the topmost surface under the cursor (a popup grab restricts it to
popups); `enter`, `leave`, `motion`, `button`, `axis` and `frame`
carry surface coordinates in 24.8 fixed point and serials. A press on
a toplevel activates it and sets the keyboard focus; the pressed
surface retains the pointer until release. The keyboard sends the keymap
as a memfd, `repeat_info (30, 500)`, `enter` with the pressed keys, `key`
with the raw code (0x80 added for the 0xe0 prefix) and `modifiers` (Shift
1, Ctrl 2, Alt 4, AltGr 16, with Caps Lock and the group in the locked and
group arguments). Compositor shortcuts: Alt+Tab cycles toplevels, Alt+F4
sends `close`, Escape dismisses a grabbed popup. `keymaps.md` describes the
layouts and their translation. X12
records a short per-client serial history so selection ownership can be
checked without accepting another client's serial.

## Data device (`data.c`)

`data_device_manager` creates sources (which `offer` MIME types) and a
data device per client. `set_selection(source, serial)` makes the
source the selection; the client with keyboard focus receives a
`data_offer` with `offer` events and `selection(offer)`, and reads it
by passing a pipe to `receive(mime, fd)`, which X12 forwards to the
source as `send(mime, fd)`. `start_drag(source, origin, icon, serial)`
validates the source, origin and initiating input serial, then the
surface under the cursor gets `data_offer`, `enter`, `motion`, `leave`
and `drop`. The normal serial is the button press; the legacy client
path may use the still-current pointer-enter serial while no button is
pressed. The target must accept the offered MIME type for a drop; `finish`
ends the drag with `dnd_finished` at the source.

X12 retains a list of the live offers. When a source is destroyed, the
offers made for it read the compositor's stored copy if the source was
the selection and a copy exists, and nothing otherwise. This matters
when the owner's window closes before its client disconnects. The
client that receives the focus gets an offer of the owner's source,
and without the list that offer would point at freed memory once the
owner is gone.

## Panel (`user/panel/`)

A layer surface anchored to the bottom (28 pixels, exclusive zone)
built directly on libwire and the libgui painter. From the left: a Menu
button with the icon `menu` that opens the launcher as a grabbed popup,
one button per toplevel from the manager (activate, minimize), the label
of the input method, the mixer, and the date and the time from a
timerfd. Text uses the interface font at 13 px.

Since B2 of `docs/plan/desktop-panel.md` the buttons are 20 pixels high
with corners of 6 pixels. A window button shows the icon of its app_id
and the title. The panel loads the icon by the name of
`launcher_icon_name`, in the colour of the text. A button is 160 pixels
wide when the windows fit, otherwise all buttons become narrower down to
40 pixels, and below 72 pixels a button shows its icon alone. Buttons
that do not fit at 40 pixels are not shown. An inactive window button
has no background. The active one has the colour `0x003f4854` and the
accent line, and a minimized one draws its icon and title dimmed. The
button under the pointer is highlighted with `0x00384049`. The function
`hit` maps a position to the part of the bar, and the drawing, the
highlight and the clicks use it. The clock shows the abbreviated weekday,
the day and the abbreviated month in the language of `LC_TIME`, and the
time without seconds (`%a %e %b %R`, with repeated spaces removed).
`kernel/tests/gui_helpers.h` repeats the geometry of `panel.h` for the
boot tests.

A click on the clock opens the calendar (`calendar.c`, B3), a popup of
264 by 236 pixels above the clock and aligned to its right edge. The
header shows the month in the form without a day (`ALTMON_1`, the
nominative in Russian) and the year between the icons `back` and
`forward`, which show the previous and the next month. The row below
contains the abbreviated weekdays (`ABDAY_1`) from the first day of the
week of the locale (`_NL_FIRST_WEEKDAY`, `locale.md`). The days of the
month follow in six rows of cells of 36 by 28 pixels, so the popup has
the same size for every month. Today has the accent colour with white
digits. A second click on the clock, a click outside the popup or Escape
closes it. The clock has the background of an open button while the
calendar is open. `panel_calendar` opens the calendar, requires the mark
of today in its cell and the background of the popup, moves one month
forward and back, and closes it.

The power button at the right end of the bar (`power.c`, B4) opens a
menu of 200 pixels with Lock, Log out, Restart and Shut down, each with
its icon (`lock`, `app-logout`, `restart`, `power-off`). The row under the
pointer is highlighted, and the account name of the session is drawn right
of Log out. Lock starts the screen locker `lock` (`lock.md`). Log out
exits the panel with status 0. This ends the session through `startgui`.
Restart and Shut down send `reboot` and `poweroff` to init with
`init_request` (`init.md`). init accepts them from the user of the session.
The entry Log out of `/etc/launcher` moved into this menu. The launcher
still treats `@logout` in a table of the user as the
row at its bottom. `gui_greeter` logs out through the power menu,
`comp_panel` opens and dismisses it, and `panel_power` boots through
init and the greeter, logs in and chooses Shut down, after which QEMU
ends with status 0.

The show desktop button of 24 pixels at the right edge of the bar (B5),
behind a line and with the icon `show-desktop`, minimizes every visible
window and records them, the active one last. A second click activates
the recorded windows in that order, so the formerly active window ends
on top. The other windows take the order of the window buttons, not their
former stacking order, which the panel does not know. A window that
opens in between ends the recorded state, and a closed window leaves
the record. The button has the background of an open button while the
desktop is shown. The panel does this through the requests `minimize`
and `activate` of the toplevel handles, without a change of the
protocol. `panel_desktop` shows the desktop with three windows, restores
them with the active window on top, and checks that a new window ends
the recorded state.

The panel is at the bottom or at the top edge of the screen (B6), as the
key `panel_position` of `desktop.conf` says (`desktop.md`). The panel
reads the key at its start and once a second with `conf_lookup` of the
libc. A change sets the anchors (top or bottom, left and right), the
exclusive zone and the size of its layer surface again. The configure
that answers the size request makes the panel commit, and X12 then lays
out the screen again (`compositor.md`). The line between the bar and the
desktop is at the edge toward the desktop. `panel_place_popup` places
every menu of the panel against its button: above it with the gravity
upwards on a bottom panel, below it with the gravity downwards on a top
panel. The launcher aligns its left edge with the Menu button, the other
menus align their right edges with their buttons. `panel_top` starts the
panel at the top, requires the desktop colour at the bottom, a window
placed and maximized below the panel and the launcher below its button,
and moves the running panel to the bottom. The buffers are allocated at the
output's scale with `set_buffer_scale`; a layer configure with a new
width (mode change) or a new output scale reallocates them, and the
panel never commits from an output event, since that would race with
the configure's serial.

The launcher menu (`launcher.c`) reads `/etc/launcher`, or the user's
`~/.config/launcher` when it exists, and the tables of installed
packages, `/var/lib/pkg/launcher` and `~/.local/share/launcher`, each
time it opens (`users.md`). The functions of `gui/launcher.h` in libgui
read the tables. The settings program and the application chooser use
the same functions. The panel starts an entry through `mime_run`.
Both files contain `title=program` lines, where the program may be
followed by arguments separated by spaces (`Screenshot=/bin/screenshot
-i`). The entries of packages are
listed under the heading Applications in the order of their titles. The
entries of `/etc/launcher` are listed under the heading System in the
order of the file. The entry with the program `@logout` is drawn in the
last row below a rule, with the account name of the panel's user at its
right end (`users.md`). The system table has no such entry since B4 of
`docs/plan/desktop-panel.md`, when Log out moved into the power menu. Each entry has a 16 pixel icon
`/usr/share/icons/app-NAME.svg`, where NAME is the file name of the
program, or `app-default.svg` when that file does not exist
(`launcher_icon_name` of libgui, which the Open with chooser uses as
well). The panel renders the icons at the output scale and caches them
by name, scale and colour (`icons.c`): dark for the menus, light for the
bar.

The first row of the menu is a search field. The popup grab gives the
menu the keyboard focus, and the panel binds a keyboard for it. Printable
characters extend the search text, and Backspace removes the last
character. The menu then lists only the entries whose title contains the
text, without regard to case, and selects the first of them. Up and Down
move the selection, and Enter starts the selected entry. The compositor
closes the menu on Escape. The menu has 6 pixels of padding, a 34 pixel
search row, 22 pixel headings and 24 pixel entry rows in 200 pixel wide
columns. Its size is computed for all entries when it opens, and the
search does not change it. When one column is taller than the screen
above the panel, Applications and System are placed in two columns. The
panel starts programs with `fork` and `execvp` and reaps them.

## Tests

`comp_shell` (configure and ack cycle, grip resize, maximize and
restore through the boxes, a title bar move, Alt+F4), `comp_seat`
(pointer enter, motion, button and axis with serials, keymap
translation of plain, shifted and extended keys, modifier events),
`comp_data` (a drag from one client to another with the payload read
through the passed pipe, and the selection), `comp_panel` (task
button minimize and restore, the launcher popup starting `clock` through
the search field). `gui_wm` starts `clock` with a click on its row.
