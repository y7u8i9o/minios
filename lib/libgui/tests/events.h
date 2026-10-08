#pragma once
/* Server messages for the host tests. WM_KEY carries input-core KEY_*
 * values, not PS/2 scancodes. Coordinates are logical pixels of the
 * window. */
#include <gui/widget.h>

struct wmsg key_msg(struct widget *win, int code, int ch, int mods);
struct wmsg mouse_msg(struct widget *win, int kind, int x, int y, int buttons);
/* A press and a release of the left button at (x, y). */
void click(struct widget *win, int x, int y);
/* One key message per byte of s. A newline is sent as KEY_ENTER. */
void type_text(struct widget *win, const char *s);
