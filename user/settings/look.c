/* Appearance, display, keyboard and mouse pages. */
#include "settings.h"
#include <gui/client.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- appearance ---- */

static const char *const modes[] = { "fill", "center", "tile", "stretch" };
static const char *const mode_names[] = { "Fill the screen", "Centre", "Tile", "Stretch" };
static const struct { const char *name; const char *path; } fonts[] = {
    { "DejaVu Sans", "/etc/fonts/DejaVuSans.ttf" },
    { "Noto Sans", "/etc/fonts/NotoSans-Regular.ttf" },
    { "Latin Modern Roman", "/etc/fonts/lmroman10-regular.otf" },
    { "Builtin bitmap font", "" },
};
#define NFONTS 4

static struct widget *wall_combo, *mode_combo, *red, *green, *blue, *swatch, *font_combo, *size_spin, *scale_combo;
static char wallpapers[16][128];
static int nwallpapers;
static int building;

static int on_wallpaper(struct widget *w, void *args, void *arg)
{
    if (building) return 1;
    int i = w->value;
    conf_set("wallpaper", i >= 0 && i < nwallpapers ? wallpapers[i] : "");
    return 1;
}
static int on_mode(struct widget *w, void *args, void *arg)
{
    if (building) return 1;
    conf_set("wallpaper_mode", modes[w->value >= 0 && w->value < 4 ? w->value : 0]);
    return 1;
}
static int on_swatch(struct widget *w, void *args, void *arg)
{
    struct painter *p = ((struct sig_paint *)args)->p;
    painter_fill(p, 0, 0, w->w, w->h, (uint32_t)(red->value << 16 | green->value << 8 | blue->value));
    painter_frame(p, 0, 0, w->w, w->h, 0x00808080);
    return 1;
}
static int on_color(struct widget *w, void *args, void *arg)
{
    widget_invalidate(swatch);
    if (building) return 1;
    char text[16];
    snprintf(text, sizeof text, "0x%02x%02x%02x", red->value, green->value, blue->value);
    conf_set("desktop_color", text);
    return 1;
}
static int on_font(struct widget *w, void *args, void *arg)
{
    if (building) return 1;
    conf_set("ui_font", fonts[w->value >= 0 && w->value < NFONTS ? w->value : 0].name);
    return 1;
}
static int on_font_size(struct widget *w, void *args, void *arg)
{
    if (building) return 1;
    conf_set_int("ui_font_px", w->value);
    return 1;
}
static int on_scale(struct widget *w, void *args, void *arg)
{
    if (building) return 1;
    static const int scales[] = { 100, 125, 150 };
    conf_set_int("ui_scale", scales[w->value >= 0 && w->value < 3 ? w->value : 0]);
    return 1;
}

static void list_wallpapers(void)
{
    nwallpapers = 0;
    DIR *d = opendir(WALLPAPER_DIR);
    if (!d)
        return;
    struct dirent *e;
    while ((e = readdir(d)) != NULL && nwallpapers < 16)
        if (e->d_name[0] != '.')
            snprintf(wallpapers[nwallpapers++], sizeof wallpapers[0], WALLPAPER_DIR "/%s", e->d_name);
    closedir(d);
}

void build_appearance(struct widget *page)
{
    building = 1;
    struct widget *grid = grid_new(page);
    widget_set_stretch(grid, 1, 0);
    grid_set_stretch(grid, -1, 1, 1);
    int r = 0;
    row_label(grid, r, "Wallpaper");
    wall_combo = combobox_new(grid);
    list_wallpapers();
    int current = nwallpapers;
    for (int i = 0; i < nwallpapers; i++) {
        const char *base = strrchr(wallpapers[i], '/');
        combobox_add(wall_combo, base ? base + 1 : wallpapers[i]);
        if (strcmp(wallpapers[i], conf_get("wallpaper")) == 0)
            current = i;
    }
    combobox_add(wall_combo, "None (solid colour)");
    combobox_select(wall_combo, current);
    widget_connect(wall_combo, "changed", on_wallpaper, NULL);
    widget_set_grid(wall_combo, r++, 1, 1, 1);
    row_label(grid, r, "Placement");
    mode_combo = combobox_new(grid);
    int mode = 0;
    for (int i = 0; i < 4; i++) {
        combobox_add(mode_combo, mode_names[i]);
        if (strcmp(modes[i], conf_get("wallpaper_mode")) == 0)
            mode = i;
    }
    combobox_select(mode_combo, mode);
    widget_connect(mode_combo, "changed", on_mode, NULL);
    widget_set_grid(mode_combo, r++, 1, 1, 1);
    uint32_t color = (uint32_t)strtoul(conf_get("desktop_color"), NULL, 0);
    row_label(grid, r, "Desktop colour");
    swatch = canvas_new(grid);
    widget_set_hint(swatch, 0, 22);
    widget_connect(swatch, "paint", on_swatch, NULL);
    widget_set_grid(swatch, r++, 1, 1, 1);
    struct widget **sliders[] = { &red, &green, &blue };
    const char *names[] = { "Red", "Green", "Blue" };
    for (int i = 0; i < 3; i++) {
        row_label(grid, r, names[i]);
        *sliders[i] = slider_new(grid, 0, 255, (int)(color >> (16 - 8 * i)) & 0xff);
        widget_set_grid(*sliders[i], r++, 1, 1, 1);
        widget_connect(*sliders[i], "changed", on_color, NULL);
    }
    widget_set_grid(separator_new(grid), r++, 0, 1, 2);
    row_label(grid, r, "Interface font");
    font_combo = combobox_new(grid);
    int fi = 0;
    for (int i = 0; i < NFONTS; i++) {
        combobox_add(font_combo, fonts[i].name);
        if (strcmp(fonts[i].name, conf_get("ui_font")) == 0)
            fi = i;
    }
    combobox_select(font_combo, fi);
    widget_connect(font_combo, "changed", on_font, NULL);
    widget_set_grid(font_combo, r++, 1, 1, 1);
    row_label(grid, r, "Font size (px)");
    size_spin = spinner_new(grid, 10, 24, conf_int("ui_font_px", 14));
    widget_connect(size_spin, "changed", on_font_size, NULL);
    widget_set_grid(size_spin, r++, 1, 1, 1);
    row_label(grid, r, "Interface scale");
    scale_combo = combobox_new(grid);
    combobox_add(scale_combo, "100 %");
    combobox_add(scale_combo, "125 %");
    combobox_add(scale_combo, "150 %");
    int sc = conf_int("ui_scale", 100);
    combobox_select(scale_combo, sc >= 150 ? 2 : sc >= 125 ? 1 : 0);
    widget_connect(scale_combo, "changed", on_scale, NULL);
    widget_set_grid(scale_combo, r++, 1, 1, 1);
    building = 0;
}

/* ---- display ---- */

/* Modes any virtio-gpu scanout accepts; the 16 MiB buffer holds up to 2560x1600. */
static const char *const resolutions[] = { "1024x768", "1280x800", "1280x1024", "1440x900", "1600x1200",
                                           "1680x1050", "1920x1080", "1920x1200", "2560x1440", "2560x1600" };
#define NRES 10
static struct widget *res_combo, *pixel_combo, *frame_spin, *decor_combo, *current_label;

static void apply_display_mode(void)
{
    if (building) return;
    if (res_combo->value >= 0 && res_combo->value < NRES) {
        char text[32];
        snprintf(text, sizeof text, "%s@%d", resolutions[res_combo->value], pixel_combo->value > 0 ? 2 : 1);
        conf_set("display_mode", text);
    }
}
static int on_res(struct widget *w, void *args, void *arg) { apply_display_mode(); return 1; }
static int on_frame(struct widget *w, void *args, void *arg)
{
    if (!building) conf_set_int("frame_ms", w->value);
    return 1;
}
static int on_decor(struct widget *w, void *args, void *arg)
{
    if (!building) conf_set("decorations", w->value == 1 ? "server" : "client");
    return 1;
}

void build_display(struct widget *page)
{
    building = 1;
    struct widget *grid = grid_new(page);
    widget_set_stretch(grid, 1, 0);
    grid_set_stretch(grid, -1, 1, 1);
    int r = 0;
    char current[64] = "unknown";
    struct gui_output_info info;
    if (gui_get_output(0, &info) == 0)
        snprintf(current, sizeof current, "%dx%d pixels, scale %d (%dx%d logical), %d Hz", info.width * info.scale,
                 info.height * info.scale, info.scale, info.width, info.height, info.refresh_hz);
    row_label(grid, r, "Current mode");
    current_label = label_new(grid, current);
    widget_set_grid(current_label, r++, 1, 1, 1);
    const char *wanted = conf_get("display_mode")[0] ? conf_get("display_mode") : current;
    int scale = strchr(wanted, '@') ? atoi(strchr(wanted, '@') + 1) : (gui_get_output(0, &info) == 0 ? info.scale : 1);
    row_label(grid, r, "Resolution");
    res_combo = combobox_new(grid);
    int selected = 0;
    for (int i = 0; i < NRES; i++) {
        combobox_add(res_combo, resolutions[i]);
        size_t n = strlen(resolutions[i]);
        if (strncmp(resolutions[i], wanted, n) == 0 && (wanted[n] == '@' || wanted[n] == ' ' || wanted[n] == '\0'))
            selected = i;
    }
    combobox_select(res_combo, selected);
    widget_connect(res_combo, "changed", on_res, NULL);
    widget_set_grid(res_combo, r++, 1, 1, 1);
    row_label(grid, r, "Pixel density");
    pixel_combo = combobox_new(grid);
    combobox_add(pixel_combo, "Standard (1 pixel per point)");
    combobox_add(pixel_combo, "High (2 pixels per point)");
    combobox_select(pixel_combo, scale >= 2 ? 1 : 0);
    widget_connect(pixel_combo, "changed", on_res, NULL);
    widget_set_grid(pixel_combo, r++, 1, 1, 1);
    widget_set_grid(separator_new(grid), r++, 0, 1, 2);
    row_label(grid, r, "Frame interval (ms)");
    frame_spin = spinner_new(grid, 4, 200, conf_int("frame_ms", 16));
    widget_connect(frame_spin, "changed", on_frame, NULL);
    widget_set_grid(frame_spin, r++, 1, 1, 1);
    row_label(grid, r, "Window decorations");
    decor_combo = combobox_new(grid);
    combobox_add(decor_combo, "Drawn by the application (client side)");
    combobox_add(decor_combo, "Drawn by X12 (server side)");
    combobox_select(decor_combo, strcmp(conf_get("decorations"), "server") == 0 ? 1 : 0);
    widget_connect(decor_combo, "changed", on_decor, NULL);
    widget_set_grid(decor_combo, r++, 1, 1, 1);
    building = 0;
}

/* ---- keyboard ---- */

static struct widget *layout_combo, *rate_slider, *delay_slider, *rate_label, *delay_label;
static char layouts[16][32];
static int nlayouts;

static void show_rates(void)
{
    char text[48];
    snprintf(text, sizeof text, "Repeat rate: %d per second", rate_slider->value);
    widget_set_text(rate_label, text);
    snprintf(text, sizeof text, "Repeat delay: %d ms", delay_slider->value);
    widget_set_text(delay_label, text);
}
static int on_rate(struct widget *w, void *args, void *arg)
{
    show_rates();
    if (!building) conf_set_int("repeat_rate", rate_slider->value);
    return 1;
}
static int on_delay(struct widget *w, void *args, void *arg)
{
    show_rates();
    if (!building) conf_set_int("repeat_delay", delay_slider->value);
    return 1;
}
static int on_layout(struct widget *w, void *args, void *arg)
{
    if (!building && w->value >= 0 && w->value < nlayouts)
        conf_set("keymap", layouts[w->value]);
    return 1;
}

void build_keyboard(struct widget *page)
{
    building = 1;
    struct widget *grid = grid_new(page);
    widget_set_stretch(grid, 1, 0);
    grid_set_stretch(grid, -1, 1, 1);
    int r = 0;
    row_label(grid, r, "Layout");
    layout_combo = combobox_new(grid);
    nlayouts = 0;
    DIR *d = opendir(KEYMAP_DIR);
    int current = 0;
    if (d) {
        struct dirent *e;
        while ((e = readdir(d)) != NULL && nlayouts < 16) {
            char *dot = strrchr(e->d_name, '.');
            if (e->d_name[0] == '.' || !dot || strcmp(dot, ".mkm") != 0)
                continue;
            *dot = '\0';
            strlcpy(layouts[nlayouts], e->d_name, sizeof layouts[0]);
            if (strcmp(layouts[nlayouts], conf_get("keymap")) == 0)
                current = nlayouts;
            combobox_add(layout_combo, layouts[nlayouts]);
            nlayouts++;
        }
        closedir(d);
    }
    combobox_select(layout_combo, current);
    widget_connect(layout_combo, "changed", on_layout, NULL);
    widget_set_grid(layout_combo, r++, 1, 1, 1);
    rate_label = label_new(grid, "");
    widget_set_grid(rate_label, r++, 0, 1, 2);
    rate_slider = slider_new(grid, 1, 100, conf_int("repeat_rate", 30));
    widget_connect(rate_slider, "changed", on_rate, NULL);
    widget_set_grid(rate_slider, r++, 0, 1, 2);
    delay_label = label_new(grid, "");
    widget_set_grid(delay_label, r++, 0, 1, 2);
    delay_slider = slider_new(grid, 50, 2000, conf_int("repeat_delay", 500));
    widget_connect(delay_slider, "changed", on_delay, NULL);
    widget_set_grid(delay_slider, r++, 0, 1, 2);
    show_rates();
    building = 0;
}

/* ---- mouse ---- */

static struct widget *speed_slider, *speed_label, *accel_combo;

static void show_speed(void)
{
    char text[48];
    snprintf(text, sizeof text, "Pointer speed: %d", speed_slider->value);
    widget_set_text(speed_label, text);
}
static int on_speed(struct widget *w, void *args, void *arg)
{
    show_speed();
    if (!building) conf_set_int("pointer_speed", speed_slider->value);
    return 1;
}
static int on_accel(struct widget *w, void *args, void *arg)
{
    if (!building && w->value >= 0)
        conf_set("pointer_accel", w->value == 0 ? "flat" : "adaptive");
    return 1;
}

void build_mouse(struct widget *page)
{
    building = 1;
    struct widget *grid = grid_new(page);
    widget_set_stretch(grid, 1, 0);
    grid_set_stretch(grid, -1, 1, 1);
    int r = 0;
    row_label(grid, r, "Acceleration");
    accel_combo = combobox_new(grid);
    combobox_add(accel_combo, "Flat");
    combobox_add(accel_combo, "Adaptive");
    combobox_select(accel_combo, strcmp(conf_get("pointer_accel"), "flat") == 0 ? 0 : 1);
    widget_connect(accel_combo, "changed", on_accel, NULL);
    widget_set_grid(accel_combo, r++, 1, 1, 1);
    speed_label = label_new(grid, "");
    widget_set_grid(speed_label, r++, 0, 1, 2);
    speed_slider = slider_new(grid, -100, 100, conf_int("pointer_speed", 0));
    widget_connect(speed_slider, "changed", on_speed, NULL);
    widget_set_grid(speed_slider, r++, 0, 1, 2);
    show_speed();
    building = 0;
}
