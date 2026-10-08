/* Server messages for the host tests (events.h). */
#include "events.h"
#include "host_compat.h"

struct wmsg key_msg(struct widget *win, int code, int ch, int mods)
{
    struct wmsg m = { .type = WM_KEY, .window = window_state_of(win)->win->id, .a = code, .b = 1, .c = mods, .d = ch };
    return m;
}

struct wmsg mouse_msg(struct widget *win, int kind, int x, int y, int buttons)
{
    struct wmsg m = { .type = WM_MOUSE, .window = window_state_of(win)->win->id, .a = x, .b = y, .c = buttons, .d = kind };
    return m;
}

void click(struct widget *win, int x, int y)
{
    struct wmsg d = mouse_msg(win, WMOUSE_DOWN, x, y, 1), u = mouse_msg(win, WMOUSE_UP, x, y, 0);
    window_message(win, &d);
    window_message(win, &u);
}

void type_text(struct widget *win, const char *s)
{
    for (; *s; s++) {
        struct wmsg m = key_msg(win, *s == '\n' ? KEY_ENTER : 0, *s, 0);
        window_message(win, &m);
    }
}

void step_for(struct app *a, int ms)
{
    long end = uptime_ms() + ms;
    for (long now = uptime_ms(); now < end; now = uptime_ms())
        app_step(a, (int)(end - now));
}

void send_key(struct widget *win, int code, int ch, int mods)
{
    struct wmsg m = key_msg(win, code, ch, mods);
    window_message(win, &m);
}

void press_key(struct widget *win, int code, int ch, int mods)
{
    struct wmsg m = key_msg(win, code, ch, mods);
    window_message(win, &m);
    m.b = 0;
    window_message(win, &m);
}

uint32_t window_pixel(struct widget *win, int x, int y)
{
    struct surface *s = &window_state_of(win)->win->surf;
    return s->pixels[(size_t)y * s->stride + x] & 0xffffff;
}
