/* Test client of the window server. Without arguments it creates two
 * windows, draws into them, and logs the events it receives until both
 * are closed. "resize" creates one window that is refilled green after
 * every resize. "clipset <text>" and "clipget" exercise the clipboard. */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <gui/client.h>

static struct gui_window *a, *b;

static void handle(struct wmsg *ev, int *keys)
{
    switch (ev->type) {
    case WM_FOCUS:
        printf("guitest: window %d focus %d\n", ev->window, ev->a);
        break;
    case WM_MOUSE:
        if (ev->d == WMOUSE_WHEEL)
            printf("guitest: window %d wheel %d\n", ev->window, ev->c);
        else if (ev->d != WMOUSE_MOVE)
            printf("guitest: window %d mouse %s at %d,%d\n", ev->window, ev->d == WMOUSE_DOWN ? "down" : "up", ev->a, ev->b);
        break;
    case WM_KEY:
        if (ev->b) {
            printf("guitest: window %d key 0x%02x char %d\n", ev->window, ev->a, ev->d);
            (*keys)++;
        }
        break;
    case WM_RESIZED: {
        struct gui_window *w = a && ev->window == a->id ? a : b;
        printf("guitest: window %d resized %dx%d\n", ev->window, ev->a, ev->b);
        if (w) {
            gfx_fill(&w->surf, gfx_rgb(0x40, 0xc0, 0x40));
            gui_damage(w, 0, 0, w->width, w->height);
        }
        break;
    }
    case WM_CLOSE:
        printf("guitest: window %d close request\n", ev->window);
        if (a && ev->window == a->id) {
            gui_destroy_window(a);
            a = NULL;
        } else if (b && ev->window == b->id) {
            gui_destroy_window(b);
            b = NULL;
        }
        break;
    }
}

int main(int argc, char **argv)
{
    if (gui_connect() < 0) {
        printf("guitest: cannot connect\n");
        return 1;
    }
    const char *mode = argc > 1 ? argv[1] : "";
    if (strcmp(mode, "clipset") == 0) {
        const char *text = argc > 2 ? argv[2] : "clip";
        /* Selection authority is an input serial. Map a real surface and
         * wait for keyboard focus instead of relying on a forged serial. */
        a = gui_create_window(80, 40, "clipboard owner");
        if (!a)
            return 1;
        gfx_fill(&a->surf, 0x00ffffff);
        gui_damage(a, 0, 0, a->width, a->height);
        struct wmsg focus;
        int focused = 0;
        for (int i = 0; i < 20 && !focused; i++)
            if (gui_next_event(&focus, 100) == 1 && focus.type == WM_FOCUS && focus.a)
                focused = 1;
        int r = gui_clipboard_set(text, (int)strlen(text));
        printf("guitest: clipboard set %s\n", r == 0 ? "ok" : "failed");
        gui_disconnect();
        return r == 0 ? 0 : 1;
    }
    if (strcmp(mode, "clipget") == 0) {
        char buf[128];
        int n = gui_clipboard_get(buf, sizeof buf);
        printf("guitest: clipboard '%s' (%d bytes)\n", n >= 0 ? buf : "", n);
        gui_disconnect();
        return n >= 0 ? 0 : 1;
    }
    if (strcmp(mode, "ttf") == 0) {
        /* One white window with a line of kerned, antialiased text. */
        struct font *f = gfx_font_open_ttf(argc > 2 ? argv[2] : "/usr/share/fonts/DejaVuSans.ttf", 32);
        if (!f) {
            printf("guitest: cannot open the outline font\n");
            return 1;
        }
        a = gui_create_window(400, 200, "ttf");
        if (!a)
            return 1;
        gfx_fill(&a->surf, 0x00ffffff);
        gfx_text_font(&a->surf, f, 10, 10, "AVAST To Wave", 0x00000000, 0xffffffffu);
        gui_damage(a, 0, 0, a->width, a->height);
        printf("guitest: ttf text width %d\n", gfx_text_width_font(f, "AVAST To Wave", -1));
        fflush(stdout);
        struct wmsg ev;
        while (gui_next_event(&ev, -1) == 1)
            if (ev.type == WM_CLOSE)
                break;
        gui_destroy_window(a);
        gui_disconnect();
        return 0;
    }
    printf("guitest: screen %dx%d\n", gui_screen_width(), gui_screen_height());
    a = gui_create_window(300, 200, "alpha");
    if (strcmp(mode, "resize") != 0)
        b = gui_create_window(240, 160, "beta");
    if (!a || (!b && strcmp(mode, "resize") != 0)) {
        printf("guitest: cannot create windows\n");
        return 1;
    }
    gfx_fill(&a->surf, gfx_rgb(220, 220, 220));
    gfx_text(&a->surf, 10, 10, "window alpha", 0, 0xffffffffu);
    if (a->scale > 1) {
        /* A device pixel checkerboard: on screen 1:1 only when the
         * compositor copies a scaled buffer without resampling. */
        for (int j = 0; j < 8; j++)
            for (int i = 0; i < 8; i++)
                a->surf.pixels[(size_t)(40 + j) * a->surf.stride + 20 + i] = ((i ^ j) & 1) ? 0x00ffffff : 0;
        printf("guitest: device checkerboard at scale %d\n", a->scale);
    }
    gui_damage(a, 0, 0, a->width, a->height);
    if (b) {
        gfx_fill(&b->surf, gfx_rgb(200, 240, 200));
        gfx_fill_rect(&b->surf, 20, 20, 100, 60, gfx_rgb(255, 0, 0));
        gui_damage(b, 0, 0, b->width, b->height);
    }
    fflush(stdout);
    struct wmsg ev;
    int keys = 0;
    while (gui_next_event(&ev, -1) == 1) {
        handle(&ev, &keys);
        fflush(stdout);
        if (!a && !b) {
            gui_disconnect();
            printf("guitest: done after %d keys\n", keys);
            return 0;
        }
    }
    return 1;
}
