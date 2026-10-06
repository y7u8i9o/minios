/* The Settings page: the settings of the running X12. */
#include <stdio.h>
#include <string.h>
#include <gui/display.h>
#include "x12settings.h"

static struct widget *frame_spin, *rate_spin, *delay_spin, *decor_combo, *verbose_box, *red, *green, *blue, *swatch;
static struct widget *mode_label;
static int applying;

void live_value(const char *key, int32_t value)
{
    applying = 1;
    if (strcmp(key, "frame_ms") == 0 && frame_spin) widget_set_value(frame_spin, value);
    else if (strcmp(key, "repeat_rate") == 0 && rate_spin) widget_set_value(rate_spin, value);
    else if (strcmp(key, "repeat_delay") == 0 && delay_spin) widget_set_value(delay_spin, value);
    else if (strcmp(key, "decorations") == 0 && decor_combo) combobox_select(decor_combo, value == 2 ? 1 : 0);
    else if (strcmp(key, "verbose") == 0 && verbose_box) { verbose_box->value = value; widget_invalidate(verbose_box); }
    else if (strcmp(key, "display_mode") == 0 && mode_label) {
        char text[64];
        snprintf(text, sizeof text, "%dx%d at scale %d", DISPLAY_MODE_W(value), DISPLAY_MODE_H(value), DISPLAY_MODE_S(value));
        widget_set_text(mode_label, text);
    } else if (strcmp(key, "desktop_color") == 0 && red) {
        widget_set_value(red, value >> 16 & 0xff);
        widget_set_value(green, value >> 8 & 0xff);
        widget_set_value(blue, value & 0xff);
        widget_invalidate(swatch);
    }
    applying = 0;
}

static void set(const char *key, int value)
{
    if (!applying)
        setting_send(key, value);
}

static int on_frame(struct widget *w, void *args, void *arg) { set("frame_ms", w->value); return 0; }
static int on_rate(struct widget *w, void *args, void *arg) { set("repeat_rate", w->value); return 0; }
static int on_delay(struct widget *w, void *args, void *arg) { set("repeat_delay", w->value); return 0; }
static int on_decor(struct widget *w, void *args, void *arg) { set("decorations", w->value == 1 ? 2 : 1); return 0; }
static int on_verbose(struct widget *w, void *args, void *arg) { set("verbose", w->value); return 0; }
static int on_keymap(struct widget *w, void *args, void *arg) { set("keymap_reload", 1); return 0; }
static int on_color(struct widget *w, void *args, void *arg)
{
    widget_invalidate(swatch);
    set("desktop_color", red->value << 16 | green->value << 8 | blue->value);
    return 0;
}
static int on_swatch(struct widget *w, void *args, void *arg)
{
    struct painter *p = ((struct sig_paint *)args)->p;
    painter_fill(p, 0, 0, w->w, w->h, (uint32_t)(red->value << 16 | green->value << 8 | blue->value));
    painter_frame(p, 0, 0, w->w, w->h, 0x00808080);
    return 1;
}

static struct widget *grid_row(struct widget *grid, int r, const char *name, const char *value)
{
    struct widget *l = label_new(grid, name);
    widget_set_hint(l, 150, 0);
    widget_set_grid(l, r, 0, 1, 1);
    struct widget *v = label_new(grid, value);
    widget_set_grid(v, r, 1, 1, 1);
    return v;
}

void live_build(struct widget *tabs)
{
    struct widget *page = tabs_add(tabs, "Settings");
    struct widget *grid = grid_new(page);
    widget_set_stretch(grid, 1, 0);
    grid_set_stretch(grid, -1, 1, 1);
    int r = 0;
    mode_label = grid_row(grid, r++, "Display mode", "");
    struct widget *l = label_new(grid, "Frame interval (ms)");
    widget_set_grid(l, r, 0, 1, 1);
    frame_spin = spinner_new(grid, 4, 200, 16);
    widget_set_grid(frame_spin, r++, 1, 1, 1);
    widget_connect(frame_spin, "changed", on_frame, NULL);
    widget_set_grid(label_new(grid, "Key repeat rate (per s)"), r, 0, 1, 1);
    rate_spin = spinner_new(grid, 1, 100, 30);
    widget_set_grid(rate_spin, r++, 1, 1, 1);
    widget_connect(rate_spin, "changed", on_rate, NULL);
    widget_set_grid(label_new(grid, "Key repeat delay (ms)"), r, 0, 1, 1);
    delay_spin = spinner_new(grid, 50, 2000, 500);
    widget_set_grid(delay_spin, r++, 1, 1, 1);
    widget_connect(delay_spin, "changed", on_delay, NULL);
    widget_set_grid(label_new(grid, "Decorations"), r, 0, 1, 1);
    decor_combo = combobox_new(grid);
    combobox_add(decor_combo, "server side");
    combobox_add(decor_combo, "client side");
    widget_set_grid(decor_combo, r++, 1, 1, 1);
    widget_connect(decor_combo, "changed", on_decor, NULL);
    widget_set_grid(label_new(grid, "Keyboard layout"), r, 0, 1, 1);
    struct widget *reload = button_new(grid, "Reload keymap");
    widget_set_grid(reload, r++, 1, 1, 1);
    widget_connect(reload, "clicked", on_keymap, NULL);
    verbose_box = checkbox_new(grid, "Log every frame and event on the console");
    widget_set_grid(verbose_box, r++, 1, 1, 1);
    widget_connect(verbose_box, "toggled", on_verbose, NULL);
    widget_set_grid(label_new(grid, "Desktop colour"), r, 0, 1, 1);
    swatch = canvas_new(grid);
    widget_set_hint(swatch, 0, 22);
    widget_set_stretch(swatch, 1, 0);
    widget_connect(swatch, "paint", on_swatch, NULL);
    widget_set_grid(swatch, r++, 1, 1, 1);
    const char *cnames[] = { "Red", "Green", "Blue" };
    struct widget **sliders[] = { &red, &green, &blue };
    int defaults[] = { 0x30, 0x60, 0x80 };
    for (int i = 0; i < 3; i++) {
        widget_set_grid(label_new(grid, cnames[i]), r, 0, 1, 1);
        *sliders[i] = slider_new(grid, 0, 255, defaults[i]);
        widget_set_grid(*sliders[i], r++, 1, 1, 1);
        widget_connect(*sliders[i], "changed", on_color, NULL);
    }
}
