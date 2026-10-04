# Input (M47)

The input subsystem has three layers: drivers that report events, the
input core in `kernel/input/` that retains device state and delivers the
events to `/dev/input/eventN` and to the console terminal, and the
compositor, which reads the nodes, moves the cursor and dispatches
keys to its clients through the seat protocol.

## Events

`kernel/include/minios/input.h` is shared with user space. An event is
`struct input_event { uint64_t time_us; uint16_t type; uint16_t code;
int32_t value; }`: a monotonic microsecond timestamp taken when the
event was reported, a type (`EV_KEY`, `EV_REL`, `EV_ABS`, `EV_SYN`), a
code and a value. Key codes are the Linux codes (`KEY_A` is 30,
`KEY_UP` is 103, `BTN_LEFT` is 0x110); the codes 1 to 88 equal the PS/2
set 1 make codes. A key value is 1 for a press, 0 for a release and 2
for a repeat. One report of a device (a mouse packet, a key change)
ends with `EV_SYN`/`SYN_REPORT`.

## Devices and drivers

A driver fills a `struct input_dev` (`kernel/include/input/input.h`)
with a name, a bus type and its capabilities (`input_set_key_cap`,
`input_set_rel_cap`, `input_set_abs_cap` with the axis range,
`input_set_repeat` for keyboards) and calls `input_register_device`,
which creates `/dev/input/eventN` in order of registration. It reports
events with `input_report_key`, `input_report_rel`, `input_report_abs`
and `input_sync`, from interrupt handlers or virtqueue completions.

- `arch/x86_64/ps2kbd.c` translates scancode set 1 to key codes: the
  unprefixed codes are their own key code, a table maps the `0xe0`
  prefixed ones, `0xe1 0x1d 0x45` is Pause and the fake shifts around
  the navigation keys are dropped. The device is "AT Translated Set 2
  keyboard", event0, with software repeat.
- `arch/x86_64/ps2mouse.c` assembles three or four byte packets (the
  IntelliMouse sequence enables the wheel) and reports `REL_X`, `REL_Y`
  (positive downwards), `REL_WHEEL` (positive away from the user) and
  the three buttons. Overflow packets are dropped.
- `drivers/usb/hid.c` (D2, `usb.md`) reports USB keyboards, mice and
  tablets behind an xHCI controller. Each HID interface is one device with
  the bus type `BUS_USB`. Its capabilities come from the boot keyboard
  format or from the parsed report descriptor.
- `drivers/virtio/virtio_input.c` accepts every virtio-input device:
  the name, the device ids, the `EV_KEY`, `EV_REL` and `EV_ABS` bitmaps
  and the absolute axis ranges come from the configuration space; the
  events of the queue are reported unchanged because virtio-input uses
  the same codes. A device with `KEY_A` is a keyboard and gets software
  repeat; QEMU's `virtio-keyboard-pci`, which `tools/run.sh` attaches
  by default, then receives the host's keys instead of the PS/2 port.
  With `CONFIG_TESTS` a "virtual tablet" device is registered after the
  real ones; `virtio_input_feed` feeds the first pointer device or the
  virtual tablet, and the GUI tests position the cursor through it.

## The core

`input/core.c` retains, per device under `input_dev.lock`, the bitmap of
keys down, the repeat state and the list of readers. `input_event`
applies the rules of evdev:

- A press of a key that is down and a release of a key that is up are
  dropped. A host that forwards its own key repeat as repeated presses
  (QEMU does) therefore produces one press; the core repeats the key
  itself.
- Any number of keys can be down at once; every change is delivered
  with the key it concerns, so a reader sees chords in full.
- A relative event with value 0 and a `SYN_REPORT` without events since
  the previous one are dropped.
- Software repeat: on a press of a device with `EV_REP` the key becomes
  the repeat key with a deadline of now plus `REP_DELAY`; the thread
  `input_repeatd` (started by `input_start_daemon`) sleeps on
  `repeat_waitq` until the earliest deadline, reports the key with
  value 2 followed by `SYN_REPORT`, and advances the deadline by
  `REP_PERIOD`. A release of the repeat key or a press of another key
  changes the state. The PS/2 keyboard repeats after 500 ms every 33 ms.

Every open descriptor of `/dev/input/eventN` is a reader with a queue
of 1024 events (four pages). A full queue is emptied and restarts with
`EV_SYN`/`SYN_DROPPED`, after which the reader fetches the key state
with `EVIOCGKEY` (the compositor releases and presses what changed).
`read` returns whole events, blocks until a report is complete and
returns `EAGAIN` with `O_NONBLOCK`; `poll` reports `POLLIN`. The ioctls
are `EVIOCGVERSION`, `EVIOCGID`, `EVIOCGNAME`, `EVIOCGCAPS` (the
capability bitmaps in one structure), `EVIOCGKEY`, `EVIOCGREP` and
`EVIOCSREP` (delay and period in ms), `EVIOCGABS(axis)` and
`EVIOCGRAB`. A grab makes the descriptor the only reader of the device
and takes its keys away from the console; closing the descriptor drops
it, so a display server that dies gives the keyboard back.

`input/keyboard.c` is the console keyboard: the keys of every
ungrabbed keyboard, with both shift, control and alt keys counted
separately (a modifier remains down until both of its keys are up), caps
lock, control characters, and repeats treated as presses. Characters go
through `tty_input_char` of `console_tty`; in raw mode the cursor,
editing and function keys become the VT escape sequences, Escape
itself is delivered and Alt prefixes a key with Escape. The old
`/dev/kbd` node and the `KBD_SCANCODES` mode no longer exist.

`devfs` gained one level of directories for `/dev/input`: a node
registered with `S_IFDIR` is a directory, and `"input/event0"`
registers under it.

## The compositor

`user/compositor/input.c` opens every `/dev/input/eventN` at start,
classifies it by its capabilities (a keyboard has `KEY_A`, a pointer
has `REL_X`, `ABS_X` or `BTN_LEFT`) and grabs it. Keys are tracked per
device and forwarded to the seat, which derives the modifier mask from
both keys of each pair (`KEYMAP_MOD_LOGO` is new for the meta keys);
repeats (value 2) are ignored because the clients repeat themselves.

The cursor position is retained in fractions of a logical pixel
(`cursor_fx`, `cursor_fy`; `cursor_x` and `cursor_y` are the floor).
Tablets place it exactly from the axis range. Relative motion is
scaled by the acceleration profile in `accel_factor`: the setting
`pointer_speed` (-100 to 100) scales every motion between 0.2 and 3
times; with `pointer_accel` set to adaptive (the default) the factor
is also multiplied by a function of the velocity in device units per
millisecond, computed from the event timestamps: 0.8 at or below one
unit per ms, 2.8 at or above eight, linear in between. Motion events
carry the fractional position in the protocol's 24.8 fixed point; the
cursor sprite is drawn at the floor. The wheel is delivered in the same
fixed point, 15 logical pixels per click, from `REL_WHEEL_HI_RES` when
a device reports it (120 units per click) and from `REL_WHEEL`
otherwise; libgui accumulates the value and reports whole clicks.

Both settings reach the compositor through the settings interface
(`pointer_speed`, `pointer_accel` 0 flat or 1 adaptive), are stored in
`/etc/desktop.conf` by the settings program's Mouse page and applied by
the desktop client like the key repeat.

## Key repeat in clients

The seat sends `repeat_info` (the `repeat_rate` and `repeat_delay`
settings). libgui (`client.c`) records the last pressed non modifier
key with the time of its first repeat; `gui_next_event` caps its poll
timeout with `gui_repeat_timeout` and queues a `WM_KEY` down message
with the translated character at every period until the release, a
keyboard leave or a new `repeat_info`. `app_step` uses the same
timeout for its own poll.

## Tests

- `input`: presses and releases through the PS/2 driver, a duplicate
  press dropped, three keys down at once, the console handler with both
  shifts, caps lock, control and an escape sequence, software repeat
  timing (200 ms then every 50 ms) reaching the console, `EVIOCGRAB`
  with a second reader refused and the console silent, the grab dropped
  by close, and `SYN_DROPPED` after an overflow.
- `mouse`, `mouse_wheel`: PS/2 packets become `REL_*` and `BTN_*`
  events; overflow and resynchronisation; three byte packets.
- `kbd`: the line discipline through the core (every key released).
- `input_tablet`: the attached virtio tablet's axis range and events.
- `input_keyboard` (`virtio-keyboard-pci` attached): the probe, the
  capabilities and keys reaching the console.
- `gui_kbd_restore`: the compositor grabs the keyboard and the mouse;
  killing it drops the grabs.
- `gui_pointer`: the compositor's cursor after slow and fast PS/2
  motion under the adaptive profile, the flat profile at speed 100, and
  fractions accumulating at speed -100.
- `gui_repeat`: a key pressed for 900 ms in evtest logs a repeat;
  `gui_tools` rejects a repeat for a key released at once.
- `gui_tablet`, `comp_seat` and the other GUI cases, which place the
  cursor through the virtual tablet.
