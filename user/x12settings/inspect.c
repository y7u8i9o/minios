/* The Inspector page: one pixel of the composed screen. */
#include <stdio.h>
#include <gui/client.h>
#include "x12settings.h"
#include "debug-client.h"

static struct widget *px_x, *px_y, *px_value, *px_swatch;
static uint32_t pixel_value;
static int have_pixel;

void inspect_pixel(int32_t x, int32_t y, uint32_t v)
{
    pixel_value = v;
    have_pixel = 1;
    if (px_value) {
        char s[64];
        snprintf(s, sizeof s, "(%d, %d) = 0x%06x  red %u, green %u, blue %u", x, y, v & 0xffffff, v >> 16 & 0xff,
                 v >> 8 & 0xff, v & 0xff);
        widget_set_text(px_value, s);
        widget_invalidate(px_swatch);
    }
}

static int on_read_pixel(struct widget *w, void *args, void *arg)
{
    debug_read_pixel(debug_proxy, px_x->value, px_y->value);
    gui_flush();
    return 1;
}

static int on_px_swatch(struct widget *w, void *args, void *arg)
{
    struct painter *p = ((struct sig_paint *)args)->p;
    painter_fill(p, 0, 0, w->w, w->h, have_pixel ? pixel_value & 0xffffff : 0x00ebebeb);
    painter_frame(p, 0, 0, w->w, w->h, 0x00808080);
    return 1;
}

void inspect_build(struct widget *tabs)
{
    struct widget *inspector = tabs_add(tabs, "Inspector");
    struct widget *grid = grid_new(inspector);
    widget_set_stretch(grid, 1, 0);
    grid_set_stretch(grid, -1, 1, 1);
    widget_set_grid(label_new(grid, "Screen x"), 0, 0, 1, 1);
    px_x = spinner_new(grid, 0, gui_screen_width() > 0 ? gui_screen_width() - 1 : 4096, 0);
    widget_set_grid(px_x, 0, 1, 1, 1);
    widget_set_grid(label_new(grid, "Screen y"), 1, 0, 1, 1);
    px_y = spinner_new(grid, 0, gui_screen_height() > 0 ? gui_screen_height() - 1 : 4096, 0);
    widget_set_grid(px_y, 1, 1, 1, 1);
    struct widget *read = button_new(grid, "Read pixel");
    widget_set_grid(read, 2, 1, 1, 1);
    widget_connect(read, "clicked", on_read_pixel, NULL);
    px_value = label_new(inspector, "");
    px_swatch = canvas_new(inspector);
    widget_set_hint(px_swatch, 0, 32);
    widget_set_stretch(px_swatch, 1, 0);
    widget_connect(px_swatch, "paint", on_px_swatch, NULL);
}
