# Screen locker

S6 of `docs/plan/release-0.7.0.md` adds a session lock to X12, the screen
locker `lock` and the setuid helper `checkpass`. The design follows the
ext-session-lock protocol of Wayland: X12 decides what the screen shows
while the session is locked, and a separate client draws the lock screen.
A failure of that client therefore leaves the session locked.

## Protocol

`protocol/lock.xml` defines three interfaces.

| Interface | Requests | Events |
|---|---|---|
| `session_lock_manager` (global) | `lock(new_id session_lock)` | |
| `session_lock` | `get_lock_surface(new_id lock_surface, surface)`, `unlock_and_destroy`, `destroy` | `locked`, `finished` |
| `lock_surface` | `ack_configure(serial)`, `destroy` | `configure(serial, width, height)` |

A client locks the session with `lock`. X12 answers with `locked` after it
has composed the first frame without the contents of the session. X12
answers with `finished` when it refuses the request. A lock has at most
one lock surface. The lock surface receives a `configure` with the size of
the screen in logical pixels. The client acknowledges the configure before
it commits a buffer of that size. A change of the display mode sends a new
configure.

`unlock_and_destroy` is valid only after `locked` and only from the lock
that owns the session. Any other use is a protocol error.
`destroy`, or the end of the connection, leaves the session locked.

## X12

`user/compositor/lock.c` contains the state of the lock:

- `locked`: the session is locked.
- `owner`: the lock request that owns the session. The value is NULL
  after the locker has exited without an unlock.
- `locker_pid`: the locker that X12 started itself, or 0.

While the session is locked, the following rules apply:

- The scene shows only the lock surface of the owner and the popups of
  that surface (`visible` in `scene.c`). The background is black instead
  of the desktop colour.
- The keyboard focus belongs to the lock surface.
  `seat_set_keyboard_focus` refuses every other surface.
- Pointer events go to the surface under the pointer. Only the lock
  surface and its popups are in the scene, so no other client receives
  them. Decorations and the not responding dialog do not react to
  presses.
- The global shortcuts are disabled: Alt+Tab, Alt+F4, the screenshot
  keys, Escape for popups, and the switch keys of the input methods. Keys
  do not reach the input method daemon. The group switch of the keyboard
  layout remains available for the password.
- Only root may copy windows with the `debug` and `screencopy`
  interfaces. A screen copy shows the lock screen.

A lock request is refused while a running locker owns the session. When
the locker has exited without an unlock, X12 shows the black screen with
the message "The screen locker has stopped. Press a key to start it
again." A key press starts a new locker. In this state X12 accepts a lock
request only from root or from the process that X12 started. X12 compares
the process id of the client from `SO_PEERCRED` at connect time
(`client.peer_pid`) with `locker_pid`. A program of the session user
therefore cannot resume an abandoned lock and unlock the session.

When the greeter ends the session (`session_uid` -1), X12 sends
`finished` to the owner and unlocks. The processes of the session end
with the session.

## Starting the locker

X12 starts `/bin/lock` in three cases:

- Super+L.
- A key press while the session is locked without a locker.
- `lock_timeout` seconds without input. Each read from an input device
  counts as input (`lock_note_input` in `input.c`). The value 0 turns the
  automatic lock off. `lock_next_deadline` gives the poll timeout of the
  main loop. The idle timeout starts one locker. A further start
  requires new input.

X12 forks the locker itself and therefore knows the process id. The child
takes the identity of the session user with `account_become`. The child
sets `HOME`, `USER`, `LOGNAME` and the language of the desktop settings.
X12 collects the exit of the locker with `waitpid` once a second and
before it starts a new locker. Without a session user (`session_uid` -1),
for example while the login window is shown, X12 starts no locker.

The panel starts `/bin/lock` from the Lock row of its power menu. This
locker is not a child of X12. A crash of this locker leads to the black
screen, and the next key starts a locker from X12.

The desktop reads `lock_timeout` from `desktop.conf` and sends it to X12
with the other settings. The default is 300 seconds. The Display page of
the Settings application offers Never, 1, 5, 10, 15 and 30 minutes and
1 hour.

## The program lock

`user/lock/lock.c` creates the lock window with `app_lock_window`. The
window has the backdrop, the top bar and the card of the greeter. The
backdrop, the card and the account row are libgui classes
(`widgets.md`). `user/greeter/screen.c` contains the top bar and the
placement of the card, and both programs link the file.
The card shows the account of the real uid, a masked password field and
the Unlock button.

Enter or Unlock runs `/bin/checkpass` with `app_run_command` and passes the
password on standard input. The window continues to handle events while
the check runs. Exit status 0 unlocks the session with `app_unlock`, and
the program exits. Any other status shows "Wrong password".

`lock` exits with status 1 when X12 refuses the lock. The program writes
`lock: locked`, `lock: wrong password` and `lock: unlocked` to standard
output for the boot tests.

libgui provides the client side:

- `gui_create_lock_window` binds `session_lock_manager`, sends `lock`,
  creates the lock surface and waits up to 5 seconds for `locked` or
  `finished`. The function returns NULL after `finished`.
- `gui_unlock_session` sends `unlock_and_destroy`.
- A `finished` event after `locked` closes the window. This happens when
  the session ends.
- `gui_destroy_window` without an unlock sends `destroy`, and the session
  remains locked.

## checkpass

`user/coreutils/checkpass.c` is installed with mode 4755, like `su` and
`passwd`. The program needs root to read `/etc/shadow`. The program reads one line
from standard input and compares the line with the hash of the account of
the real uid (`account_check`). A wrong password costs 2 seconds before
the program exits with status 1. Each call checks one password. A program
of the user therefore tests at most one password every 2 seconds per
process.

## Changes outside the lock

- libwire: a failed send no longer stops the reads of the connection. A
  client may send a request and close the connection while X12 still
  writes to it. The write then fails with `EPIPE`. Before, the failure
  marked the connection as broken, and X12 dropped the requests that the
  client had sent before it closed. The unlock request of `lock` was lost
  in this way. `wire_conn.send_failed` now records the failed send. The
  output is dropped, and the reads continue until the end of the input.
- libgui: the text field clamps its cursor and selection anchor to the
  length of the text before it paints or handles an event.
  `widget_set_text` can shorten the text, and the old cursor offset then
  pointed past the end of the text. `lock` cleared its password field
  after a wrong password and crashed on the next paint.
- The panel answers the pings of X12. Before, the panel bound the `shell`
  global without a listener, and X12 marked the panel as not responding three
  seconds after the panel started.
- libc: `account_become` (`minios/account.h`) sets the groups, the gid and
  the uid of an account. `login`, the greeter and X12 use this function.

## Tests

- `gui_lock` starts X12, the panel and `guitest`, a client that logs its
  keys. It locks the session with Super+L and checks that the panel is
  hidden. A click on the power button, a second Super+L and a second
  locker started by root change nothing. A wrong password leaves the
  session locked. The right password unlocks the session. The test
  window received no key while the session was locked. The window receives
  keys again after the unlock. Finally the test locks the session from the
  power menu and unlocks it. The QMP script saves a screendump of the lock
  screen.
- `gui_lock_crash` kills the locker with SIGKILL and checks the black
  screen. A locker of the session user started with `doas` is refused. A
  key press starts a new locker. This locker unlocks the session with the
  password.
- `lock_idle` sets `lock_timeout` to 3 seconds and waits for the locker.
  Pointer motion every second defers the lock. The value 0 turns the
  automatic lock off.
