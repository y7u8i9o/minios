# X12 shell, seat, data device and panel

M25 gives surfaces roles and delivers input and data transfers; X12 keeps
the protocol interface names stable for clients.
Protocol definitions: `protocol/shell.xml`, `seat.xml`, `data.xml`;
compositor modules: `user/compositor/shell.c`, `decor.c`, `seat.c`,
`data.c`; the panel: `user/panel/panel.c`.

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
  `configure(serial, w, h)` follows `set_size`.
- `shell.get_decoration(toplevel)` with `set_mode` (1 server, 2
  client): the compositor draws title bars, boxes and the resize grip
  only in server mode.
- `toplevel_manager` (a global): every bound manager receives a
  `toplevel_handle` per toplevel with `title`, `app_id` and `state`
  events (maximized 1, activated 2, minimized 3) and `closed`; handles
  accept `activate`, `minimize` and `close`.

## Decorations (`decor.c`)

Server side decorations are drawn into the scene around the surface
(title bar of 20 pixels, a border, close, maximize and minimize boxes,
a grip in the bottom right corner of the contents). Presses on them
never reach the client: the title bar moves, the boxes act, the grip
resizes on release with a configure of the new size. Alt with the left
button moves from anywhere.

## Seat (`seat.c`)

One seat with pointer and keyboard capabilities. The pointer focus is
the topmost surface under the cursor (a popup grab restricts it to
popups); `enter`, `leave`, `motion`, `button`, `axis` and `frame`
carry surface coordinates in 24.8 fixed point and serials. A press on
a toplevel activates it and sets the keyboard focus; the pressed
surface keeps the pointer until release. The keyboard sends the keymap
as a memfd (`/usr/share/keymaps/us.mkm` copied at start), `repeat_info
(30, 500)`, `enter` with the pressed keys, `key` with the raw code
(0x80 added for the 0xe0 prefix) and `modifiers` (Shift 1, Ctrl 2,
Alt 4). Compositor shortcuts: Alt+Tab cycles toplevels, Alt+F4 sends
`close`, Escape dismisses a grabbed popup. Keymaps
(`tools/genkeymap/genkeymap.py`) hold four levels per key code;
`libgui/include/gui/keymap.h` loads them and translates codes. X12
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

## Panel (`user/panel/`)

A layer surface anchored to the bottom (28 pixels, exclusive zone)
built directly on libwire and gfx: a Menu button opening the launcher
as a grabbed popup, one button per toplevel from the manager
(activate, minimize), a clock from a timerfd. It launches programs
from `/etc/launcher` and reaps them.

## Tests

`comp_shell` (configure and ack cycle, grip resize, maximize and
restore through the boxes, a title bar move, Alt+F4), `comp_seat`
(pointer enter, motion, button and axis with serials, keymap
translation of plain, shifted and extended keys, modifier events),
`comp_data` (a drag from one client to another with the payload read
through the passed pipe, and the selection), `comp_panel` (task
button minimize and restore, the launcher popup starting `clock`).
