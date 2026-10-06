# Notifications

Milestone S5 of `docs/plan/release-0.7.0.md` lets programs post desktop
notifications. The data model follows the freedesktop notification
specification. As in GNOME, the work is divided between a session daemon
and the shell, which here is the panel.

## Components

- `notifyd` (`user/notifyd/notifyd.c`) owns the notifications. It serves
  the protocol `notify` (`protocol/notify.xml`) on the socket `notify`. It
  records the history of the session, expires pop-ups, applies the
  do-not-disturb state and delivers actions to the programs that posted
  the notifications. It draws nothing, so it also runs without a display,
  as it does in the protocol test.
- The panel (`user/panel/notify.c`) is the display of `notifyd`. It draws
  the pop-ups and the history and sends the user's clicks back to the
  daemon.
- libgui (`gui/notify.h`, `src/notify.c`) provides the client side.
  `gui_notify` posts a single notification. A `notify_client` posts several
  notifications and receives their actions and closing events.
- `notify-send` posts notifications from the shell.

`startgui` starts `notifyd` before the panel and restarts it up to three
times if it fails. If `notifyd` is not running, the panel tries to connect
once a second.

## Protocol

`notify_manager` is the global of the socket. A client creates a
`notification` with `create` (application name, summary and body), sets
optional properties and then sends `show`. The properties are:

- `set_icon`: the name of an icon in `/usr/share/icons`.
- `set_urgency`: 0 for low, 1 for normal (the default) and 2 for critical.
- `set_timeout`: a time in milliseconds. The value -1 selects the default
  of 5 seconds, and 0 means that the pop-up does not expire. Critical
  notifications do not expire by default.
- `add_action`: a key and a label, for up to three actions. The key
  `default` stands for a click on the notification itself.
- `set_replaces`: the number of a notification to replace.

`notifyd` answers `show` with `shown` and the number of the notification.
The number is unique while `notifyd` runs. A replacement retains the number
and the position in the history, and the previous sender learns through
`closed` that its notification was replaced. The notification then sends
two kinds of events. `action` carries the key of the invoked action.
`closed` carries a reason: 1 if the pop-up expired, 2 if the user dismissed
it or invoked an action, 3 if the sender closed it and 4 if the user
removed it from the history. The request `close` removes the notification.
When the client disconnects, its notifications remain in the history, but
`notifyd` can no longer deliver their actions.

`get_display` creates a `notify_display`. A new display first receives the
do-not-disturb state and then every notification in the history, oldest
first, together with the state of its pop-up. Afterwards it receives these
events:

- `notification` for new or replaced content, with the actions encoded as
  lines of key and label;
- `popup` when a pop-up appears or disappears;
- `removed` and `dnd`.

The requests of a display are the actions of the user: `invoke`,
`dismiss`, `remove`, `clear` and `set_dnd`.

## Behaviour

- The history holds at most 50 notifications. When a new notification
  arrives, `notifyd` removes the oldest one.
- A pop-up closes when its timeout expires, when the user dismisses it or
  when the user invokes an action. The notification remains in the history.
- With do-not-disturb on, only critical notifications show pop-ups. The
  others enter the history silently. Turning do-not-disturb on also closes
  the visible pop-ups of non-critical notifications.
- `notifyd` logs each event on standard output unless it runs with `-q`.

## Panel

The pop-ups are cards 340 pixels wide. The panel shows at most four, newest
first, stacked downwards from the top right corner of the desktop area.
Each card is an opaque layer surface of its own in the top layer. The panel
positions the cards through the top margins of their layer surfaces. The
compositor measures these margins from the desktop area, so the cards clear
a panel at the top of the screen.

A card shows the application name, a close button, the summary and up to
three lines of the body, wrapped with the new libgui function
`painter_wrap`. It also shows a button for each action other than
`default`. A critical notification has a red bar at the left edge of its
card. A click on the card invokes `default` if the notification has that
action, and otherwise dismisses the notification.

The bell button left of the clock opens the history popup. The popup shows
the six newest notifications, each with its time and a remove button. It
also contains the button "Clear all" and the do-not-disturb switch. If
notifications have arrived that the user has not yet seen in the history,
the bell shows a dot in the accent colour. With do-not-disturb on, the bell
is crossed out. The bell button moves the mixer and the input method label
34 pixels to the left. The icons `notifications`, `notifications-off` and
`close` come from the Font Awesome icons `bell`, `bell-slash` and `xmark`.

## notify-send

    notify-send [-a app] [-i icon] [-u low|normal|critical] [-t ms]
                [-r number] [-A key=label]... [-p] [-w] summary [body]

`-p` prints the number of the notification. `-w` waits until the
notification closes. It prints `action KEY` for each invoked action and
then `closed REASON`, where REASON is `expired`, `dismissed`, `closed` or
`removed`.

## Tests

- `notify_proto` runs `notifytest`, which starts `notifyd` and acts as both
  a client and a display. It checks numbering, replacement, closing by the
  sender, expiry, actions, dismissal, removal, the do-not-disturb state,
  the initial state of a new display, the history limit and clearing.
- `gui_notify` starts `notifyd`, X12 and the panel. It posts a notification
  with two actions through `notify-send -w`, clicks an action button and
  checks the output of `notify-send`. It then checks an expiring pop-up, the
  close button, the history popup above the bell and the do-not-disturb
  switch. Its QMP script saves screendumps of a pop-up and of the history.
