# x12settings

`x12settings` (`user/apps/compsettings.c`, installed as
`/usr/bin/x12settings`) shows the state of the running X12 and changes
its settings until X12 exits. The persistent choices of the user belong
to the Settings program. `x12settings set KEY VALUE` changes one setting
without a window. The plan of the tool is `docs/plan/x12settings.md`.

## Shared building blocks (X1)

The tool uses parts that other programs share:

- the process table of libc (`minios/proctab.h`, `libc.md`), which gives
  the program of a client,
- the graph widget, the colour dialog and the colour button of libgui
  (`framework.md`),
- the display modes of `gui/display.h`.

Before X1, `sysmon` drew its graphs with private code, seven programs
and tests parsed `/dev/proc` each in their own way, and X12,
`x12settings`, the desktop and the Settings program each contained the
packed display mode or the list of resolutions.
