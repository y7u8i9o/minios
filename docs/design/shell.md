# X12 shell, seat, data device and panel

M25 gives surfaces roles and delivers input and data transfers; X12 keeps
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
  a buffer of that size. The first commit with a buffer places the
  window in a cascade inside the desktop area and activates it.
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
  instead of the screen (`compositor.md`).
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
`libgui/src/csd.c`, see `gui.md`), and the server only composites,
places the window and drives its move and resize drags. A client asks
for that through `shell.get_decoration` and `decoration.set_mode(2)`;
the server grants the client's choice and answers with `mode`. Clients
that never ask (comptest) get the mode of the `decorations` setting,
server side by default.

`toplevel.set_window_geometry(x, y, w, h)` names the visible window
inside the surface, without the client's shadow margins. The server
keeps it in `struct toplevel.geo` and works on the visible frame,
`toplevel_frame` (the server decorations, the geometry, or the surface):
configure sizes are geometry sizes (`toplevel_configure_size`), the
cascade puts the frame of the n-th new window at (40, 24) plus n times
(30, 30) of the desktop area, so that the contents under a 36 pixel
header bar start at y 60; a modal child is centred on its parent's
frame; `clamp_toplevel` keeps 40 pixels of the frame and its top row on
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
moves, the buttons act, an invisible `RESIZE_MARGIN` around the frame and
the bottom right grip of the contents resize on release with a configure
of the new size; margins near a corner take both edges. Alt with the
left button moves any toplevel from anywhere, whichever side decorates.

A move or resize the client requested (`toplevel.move`, `toplevel.resize`
with the press serial) is driven by the same drag code, but the release
that ends it is still delivered to the client, so its button state stays
consistent (`decor_release` returns 2 for such drags). A move or resize
request is honoured only while a pointer button is held: one that
arrives after the release, for example from a client that answered late,
is ignored instead of starting a drag with no button held. Popup grabs
keep accepting the press serial after the release, as the panel needs.

## Seat (`seat.c`)

One seat with pointer and keyboard capabilities. The pointer focus is
the topmost surface under the cursor (a popup grab restricts it to
popups); `enter`, `leave`, `motion`, `button`, `axis` and `frame`
carry surface coordinates in 24.8 fixed point and serials. A press on
a toplevel activates it and sets the keyboard focus; the pressed
surface keeps the pointer until release. The keyboard sends the keymap
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
held. The target must accept the offered MIME type for a drop; `finish`
ends the drag with `dnd_finished` at the source.

X12 keeps a list of the live offers. When a source is destroyed, the
offers made for it read the compositor's stored copy if the source was
the selection and a copy exists, and nothing otherwise. This matters
when the owner's window closes before its client disconnects. The
client that receives the focus gets an offer of the owner's source,
and without the list that offer would point at freed memory once the
owner is gone.

## Panel (`user/panel/`)

A layer surface anchored to the bottom (28 pixels, exclusive zone)
built directly on libwire and the libgui painter: a Menu button opening
the launcher as a grabbed popup, one pill shaped button per toplevel
from the manager (activate, minimize; the active one carries an accent
underline, minimized ones dim their title), a clock from a timerfd.
Text uses the interface font at 13 px. The buffers are allocated at the
output's scale with `set_buffer_scale`; a layer configure with a new
width (mode change) or a new output scale reallocates them, and the
panel never commits from an output event, since that would race with
the configure's serial.

The launcher menu (`launcher.c`) reads `/etc/launcher`, or the user's
`~/.config/launcher` when it exists, and the tables of installed
packages, `/usr/local/share/launcher` and `~/.local/share/launcher`, each
time it opens (`users.md`).
Both files contain `title=program` lines, where the program may be
followed by arguments separated by spaces (`Screenshot=/bin/screenshot
-i`). The entries of packages are
listed under the heading Applications in the order of their titles. The
entries of `/etc/launcher` are listed under the heading System in the
order of the file. The entry with the program `@logout` is drawn in the
last row below a rule, with the account name of the panel's user at its
right end (`users.md`). Each entry has a 16 pixel icon
`/usr/share/icons/app-NAME.svg`, where NAME is the file name of the
program, or `app-default.svg` when that file does not exist. The panel
renders the icons at the output scale and caches them by name.

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
