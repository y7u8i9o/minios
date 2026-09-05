/* x12settings: the window server's status, surfaces, live settings and a
 * pixel inspector through the settings and debug protocol interfaces.
 * "x12settings set KEY VALUE" applies one setting without a window. The
 * user's persistent choices live in the Settings program; this tool
 * changes the running server. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <gui/app.h>
#include <gui/client.h>
#include <gui/model.h>
#include "debug-client.h"

/* display_mode packs width, height and the pixel scale as in the compositor. */
#define DISPLAY_MODE_W(m) (((m) >> 14) & 0x3fff)
#define DISPLAY_MODE_H(m) ((m) & 0x3fff)
#define DISPLAY_MODE_S(m) (((m) >> 28) & 7)

struct surf_row { unsigned id, client, mapped, format; char role[16], title[48]; int x, y, w, h; };

static struct app *app;
static struct wire_proxy *settings, *debug;
static struct widget *frame_spin, *rate_spin, *delay_spin, *decor_combo, *verbose_box, *red, *green, *blue, *swatch;
static struct widget *mode_label;
static struct widget *status_values[8], *table, *surface_count;
static struct widget *px_x, *px_y, *px_value, *px_swatch;
static struct surf_row rows[64];
static int nrows, pending_rows;
static int applying;
static uint32_t pixel_value;
static int have_pixel;

/* ---- settings events ---- */

static void on_value(void *user, struct wire_proxy *p, const char *key, int32_t value)
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
static void on_settings_done(void *user, struct wire_proxy *p) {}
static const struct settings_listener settings_events = { on_value, on_settings_done };

static void set(const char *key, int value)
{
    if (applying || !settings)
        return;
    settings_set(settings, key, value);
    gui_flush();
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

/* ---- debug events ---- */

static void on_stats(void *user, struct wire_proxy *p, uint32_t uptime, uint32_t comps, uint32_t ms, uint32_t max,
                     uint32_t clients, uint32_t surfaces, uint32_t frame_ms)
{
    char s[64];
    if (!status_values[0])
        return;
    snprintf(s, sizeof s, "%u:%02u:%02u", uptime / 3600000, uptime / 60000 % 60, uptime / 1000 % 60);
    widget_set_text(status_values[0], s);
    snprintf(s, sizeof s, "%u", comps);
    widget_set_text(status_values[1], s);
    snprintf(s, sizeof s, "%u ms total, %.2f ms average", ms, comps ? (double)ms / comps : 0.0);
    widget_set_text(status_values[2], s);
    snprintf(s, sizeof s, "%u ms", max);
    widget_set_text(status_values[3], s);
    snprintf(s, sizeof s, "%u", clients);
    widget_set_text(status_values[4], s);
    snprintf(s, sizeof s, "%u", surfaces);
    widget_set_text(status_values[5], s);
    snprintf(s, sizeof s, "%u ms (%u per second)", frame_ms, frame_ms ? 1000 / frame_ms : 0);
    widget_set_text(status_values[6], s);
    struct gui_output_info info;
    if (gui_get_output(0, &info) == 0) {
        snprintf(s, sizeof s, "%dx%d at scale %d, %d Hz", info.width * info.scale, info.height * info.scale, info.scale,
                 info.refresh_hz);
        widget_set_text(status_values[7], s);
    }
    printf("x12settings: up %u s, %u compositions (%u ms, max %u ms), %u clients, %u surfaces, frame %u ms\n",
           uptime / 1000, comps, ms, max, clients, surfaces, frame_ms);
    fflush(stdout);
}
static void on_surface(void *user, struct wire_proxy *p, uint32_t id, uint32_t client, const char *role, const char *title,
                       int32_t x, int32_t y, int32_t w, int32_t h, uint32_t mapped, uint32_t format)
{
    if (pending_rows >= 64)
        return;
    struct surf_row *r = &rows[pending_rows++];
    r->id = id; r->client = client; r->x = x; r->y = y; r->w = w; r->h = h; r->mapped = mapped; r->format = format;
    strlcpy(r->role, role, sizeof r->role);
    strlcpy(r->title, title, sizeof r->title);
}
static void on_surfaces_done(void *user, struct wire_proxy *p)
{
    nrows = pending_rows;
    pending_rows = 0;
    if (table) {
        view_refresh(table);
        char s[48];
        snprintf(s, sizeof s, "%d surface%s", nrows, nrows == 1 ? "" : "s");
        widget_set_text(surface_count, s);
    }
}
static void on_pixel(void *user, struct wire_proxy *p, int32_t x, int32_t y, uint32_t v)
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
static const struct debug_listener debug_events = { on_stats, on_surface, on_surfaces_done, on_pixel };

static int m_rows(struct model *m, int parent) { return parent < 0 ? nrows : 0; }
static int m_child(struct model *m, int parent, int index) { return index; }
static int m_columns(struct model *m) { return 6; }
static const char *m_cell(struct model *m, int row, int col, char *buf, size_t size)
{
    struct surf_row *r = &rows[row];
    switch (col) {
    case 0: snprintf(buf, size, "%u", r->id); return buf;
    case 1: snprintf(buf, size, "%u", r->client); return buf;
    case 2: return r->role;
    case 3: return r->title;
    case 4: snprintf(buf, size, "%d,%d %dx%d", r->x, r->y, r->w, r->h); return buf;
    default: snprintf(buf, size, "%s%s", r->mapped ? "mapped" : "hidden", r->format == 2 ? " argb" : r->format == 1 ? " xrgb" : ""); return buf;
    }
}
static const char *m_header(struct model *m, int col)
{
    static const char *const names[] = { "Id", "Client", "Role", "Title", "Geometry", "State" };
    return names[col];
}
static struct model model = { m_rows, m_child, m_columns, m_cell, m_header, NULL, NULL, NULL };

static void tick(void *arg)
{
    if (debug) {
        debug_get_stats(debug);
        debug_get_surfaces(debug);
        gui_flush();
    }
}
static int on_refresh(struct widget *w, void *args, void *arg) { tick(NULL); return 1; }
static int on_read_pixel(struct widget *w, void *args, void *arg)
{
    debug_read_pixel(debug, px_x->value, px_y->value);
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

static struct widget *grid_row(struct widget *grid, int r, const char *name, const char *value)
{
    struct widget *l = label_new(grid, name);
    widget_set_hint(l, 150, 0);
    widget_set_grid(l, r, 0, 1, 1);
    struct widget *v = label_new(grid, value);
    widget_set_grid(v, r, 1, 1, 1);
    return v;
}

static struct widget *page_grid(struct widget *page)
{
    struct widget *grid = grid_new(page);
    widget_set_stretch(grid, 1, 0);
    grid_set_stretch(grid, -1, 1, 1);
    return grid;
}

int main(int argc, char **argv)
{
    app = app_create();
    if (!app)
        return 1;
    settings = gui_bind_global("settings", &settings_interface, 1);
    debug = gui_bind_global("debug", &debug_interface, 1);
    if (!settings || !debug) {
        fprintf(stderr, "x12settings: X12 has no settings interface\n");
        return 1;
    }
    settings_add_listener(settings, &settings_events, NULL);
    debug_add_listener(debug, &debug_events, NULL);
    if (argc == 4 && strcmp(argv[1], "set") == 0) {
        settings_set(settings, argv[2], atoi(argv[3]));
        gui_flush();
        struct wmsg ev;
        gui_next_event(&ev, 200);
        printf("x12settings: set %s %s\n", argv[2], argv[3]);
        fflush(stdout);
        app_destroy(app);
        return 0;
    }
    struct widget *win = app_window(app, 620, 440, "X12 settings");
    if (!win)
        return 1;
    struct widget *tabs = tabs_new(win);
    widget_set_stretch(tabs, 1, 1);

    struct widget *status = tabs_add(tabs, "Status");
    struct widget *grid = page_grid(status);
    static const char *const names[8] = { "Uptime", "Compositions", "Composition time", "Longest composition",
                                          "Clients", "Surfaces", "Frame interval", "Display" };
    for (int i = 0; i < 8; i++)
        status_values[i] = grid_row(grid, i, names[i], "");

    struct widget *surfaces = tabs_add(tabs, "Surfaces");
    struct widget *bar = toolbar_new(surfaces);
    widget_connect(button_new(bar, "Refresh"), "clicked", on_refresh, NULL);
    surface_count = label_new(bar, "");
    table = table_new(surfaces);
    view_set_model(table, &model);
    table_set_column_width(table, 0, 40);
    table_set_column_width(table, 1, 50);
    table_set_column_width(table, 2, 70);
    table_set_column_width(table, 3, 150);
    table_set_column_width(table, 4, 150);
    table_set_column_width(table, 5, 90);
    widget_set_stretch(table, 1, 1);

    struct widget *page = tabs_add(tabs, "Settings");
    grid = page_grid(page);
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
    struct widget *reload = button_new(grid, "Reload from /etc/desktop.conf");
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

    struct widget *inspector = tabs_add(tabs, "Inspector");
    grid = page_grid(inspector);
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

    settings_get_all(settings);
    tick(NULL);
    app_timer_add(app, 1000, 1, tick, NULL);
    app_run(app);
    app_destroy(app);
    return 0;
}
