# Session manager

`sessiond` (`user/sessiond/sessiond.c`) decides the actions of the
graphical session. X12 detects the events of the session and reports them
through the `session_manager` interface of `protocol/session.xml`. X12
starts no program and ends no process itself. Before the split of
2026-10-30, X12 started `/bin/lock` and `/bin/screenshot` and killed
unresponsive clients itself (`compositor.md`).

## Division of work

| Event | X12 | sessiond |
|---|---|---|
| Super+L | sends `shortcut "lock"` | starts `/bin/lock` |
| Print Screen and Super+Shift+3, 4, 5 | sends `shortcut` | starts `/bin/screenshot` |
| `lock_timeout` seconds without input | sends `idle` | starts `/bin/lock` |
| A key in a locked session without a locker | sends `locker_needed` | starts `/bin/lock` |
| A client stops answering pings | dims the toplevels of the client, sends `client_state` | shows the dialog with Wait and Force quit |
| Force quit | disconnects the client on `drop_client` | sends `SIGKILL` to the process |

X12 retains every decision that protects the session. X12 enforces the
session lock, admits clients by their uid, and accepts the lock request of
a new locker only from root or from the process that `sessiond` reported
with `set_locker` (`lock.md`).

## The interface

`session.c` in `user/compositor/` implements the global. Only a client of
root or of the user that runs X12 may bind the global. X12 posts a
protocol error to any other client.

- `set_user(uid)` sets the session user, -1 for none. The greeter sends
  the request through `x12settings session-user UID` (`users.md`). The
  request replaces the former setting `session_uid`. X12 disconnects the
  clients of a previous session user, ends a session lock when the session
  ends, and sends the event `user` to every session manager. X12 also sends
  `user` when the global is bound.
- `bind_shortcut(key, mods, name)` binds a key with exactly the modifiers
  `mods`, the `KEYMAP_MOD_*` bits of Shift, Ctrl, Alt and Super. A press of
  the key sends `shortcut(name)`, and no client receives the key or its
  release. X12 activates no shortcut while the session is locked or while
  an overlay layer surface has the keyboard focus. The screenshot interface
  therefore receives Print Screen as its capture key. A second binding of
  the same key and modifiers replaces the first one. The bindings of a
  session manager end with its object.
- `idle` follows `lock_timeout` seconds without input while a session user
  exists and the session is unlocked. X12 sends the event once for each
  period without input. The setting `lock_timeout` remains a setting of
  X12, because X12 reads the input devices.
- `locker_needed` follows a key press in a locked session whose locker has
  exited.
- `set_locker(pid)` reports the process of a locker that `sessiond`
  started.
- `client_state(number, pid, responding, title)` reports a client that
  stopped (`responding` 0) or resumed (`responding` 1) answering pings. An
  unresponsive client that disconnects also produces `responding` 1. The
  title is the title of the topmost toplevel of the client.
- `drop_client(number)` disconnects a client.

## Shortcuts

`sessiond` binds six keys at start.

| Key | Name | Action |
|---|---|---|
| Super+L | `lock` | `/bin/lock` |
| Print Screen | `screenshot-interface` | `/bin/screenshot -i` |
| Shift+Print Screen | `screenshot-screen` | `/bin/screenshot -t` |
| Super+Shift+3 | `screenshot-screen` | `/bin/screenshot -t` |
| Super+Shift+4 | `screenshot-area` | `/bin/screenshot -a` |
| Super+Shift+5 | `screenshot-interface` | `/bin/screenshot -i` |

## Programs

`start_program` forks the program. The child takes the identity of the
session user with `account_become`, sets `HOME`, `USER`, `LOGNAME` and the
language of the desktop settings, and runs the program. Without a session
user, `sessiond` starts no locker. The screenshot program then runs as the
user of `sessiond`.

The child of a locker waits on a pipe before the `exec`. The parent first
sends `set_locker` with the pid of the child and flushes the connection.
The parent then writes to the pipe. X12 therefore knows the pid before the
locker connects. `sessiond` collects the exits of its children once a
second and before it starts a locker. A second locker does not start while
the first one runs.

## The dialog of an unresponsive client

X12 dims the contents of every toplevel of an unresponsive client
(`wm_draw_above` in `wm/window.c`). A click on the dimmed contents reaches
no surface. The title bar remains usable.

`sessiond` shows one dialog at a time. The dialog is a layer window on
the top layer (2), centred on the screen, with keyboard interactivity. A
click gives the dialog the keyboard focus. The dialog shows "TITLE is not
responding." with the buttons Wait and Force quit. Wait has the keyboard
focus. Tab moves the focus, and Enter or Space presses the focused button.

- Wait hides the dialog of the client for fifteen seconds. The window
  remains dimmed until the client answers.
- Force quit sends `SIGKILL` to the pid of the client and sends
  `drop_client`. A client without a known pid is only disconnected.

The dialog of the next unresponsive client follows. A client that answers
again or disconnects closes its dialog.

## Start and end

The greeter starts `sessiond` as root after X12 accepts clients. The
greeter restarts `sessiond` after an abnormal exit. `startgui` without
`-s` starts X12 and then `sessiond` as the user of `startgui`, and
restarts `sessiond` up to three times. The boot tests start `sessiond`
in `start_server` of `kernel/tests/gui_helpers.h`.

`sessiond` is a libgui program without a window. `app_step` returns 0 while
a program has no window, so `sessiond` runs `app_step` until
`gui_connection_lost` reports the end of the connection. `sessiond` then
writes `sessiond: the display server ended` and exits with status 0.

## Log

`sessiond` writes one line per action on standard output: `started`,
`session uid N`, `locker N started (Super+L|idle|key press)`, `locker N
runs already`, `locker N exited with status S`, `screenshot started`, `no
session for PROGRAM`, `dialog of client N shown`, `dialog of client N
closed`, `client N: wait` and `client N force quit`. Every line starts
with `sessiond: `. X12 writes `shortcut NAME bound`, `shortcut NAME`,
`session idle for N s`, `a locker is needed`, `locker N reported` and
`client N dropped by the session manager`.

## Tests

- `comp_hang`: the dimming, the dialog, Wait with the recovery of the
  client, and Force quit through the keyboard.
- `gui_lock`, `gui_lock_crash`: Super+L, the black screen after a crash of
  the locker, the refusal of a foreign locker, and the new locker after a
  key press.
- `lock_idle`: the locker after `lock_timeout` seconds.
- `gui_images`: Print Screen and Super+Shift+3.
