/* The Settings page: every setting of the running X12. A control sends
 * its value at once. The controls follow the values that X12 reports,
 * also when another client changes them. */
#include <stdio.h>
#include <string.h>
#include <gui/display.h>
#include "x12settings.h"
#include "debug-client.h"

#define MAX_METHODS 16

static struct widget *mode_label, *res_combo, *scale_combo, *follow_box, *frame_spin;
static struct widget *rate_spin, *delay_spin, *speed_slider, *accel_combo;
static struct widget *method_combo, *shift_box, *ctrl_space_box, *decor_combo, *color_button, *verbose_box;
static const char *const *resolutions;
static int nres, applying;
static char method_names[MAX_METHODS][32], pending_names[MAX_METHODS][32];
static int nmethods, npending, current_method, pending_current;

/* ---- values from X12 ---- */

static void set_box(struct widget *w, int value)
{
    if (w && w->value != (value != 0)) {
        w->value = value != 0;
        widget_invalidate(w);
    }
}

void live_value(const char *key, int32_t value)
{
    applying = 1;
    if (strcmp(key, "display_mode") == 0 && mode_label) {
        char text[64], mode[32];
        display_mode_format(value, mode, sizeof mode);
        snprintf(text, sizeof text, "%dx%d at scale %d", DISPLAY_MODE_W(value), DISPLAY_MODE_H(value),
                 DISPLAY_MODE_S(value));
        widget_set_text(mode_label, text);
        for (int i = 0; i < nres; i++)
            if (strncmp(resolutions[i], mode, strlen(resolutions[i])) == 0 && mode[strlen(resolutions[i])] == '@')
                combobox_select(res_combo, i);
        combobox_select(scale_combo, DISPLAY_MODE_S(value) >= 2 ? 1 : 0);
    } else if (strcmp(key, "display_follow") == 0) set_box(follow_box, value);
    else if (strcmp(key, "frame_ms") == 0 && frame_spin) widget_set_value(frame_spin, value);
    else if (strcmp(key, "repeat_rate") == 0 && rate_spin) widget_set_value(rate_spin, value);
    else if (strcmp(key, "repeat_delay") == 0 && delay_spin) widget_set_value(delay_spin, value);
    else if (strcmp(key, "pointer_speed") == 0 && speed_slider) widget_set_value(speed_slider, value);
    else if (strcmp(key, "pointer_accel") == 0 && accel_combo) combobox_select(accel_combo, value == 1 ? 1 : 0);
    else if (strcmp(key, "ime_shift_toggle") == 0) set_box(shift_box, value);
    else if (strcmp(key, "ime_ctrl_space") == 0) set_box(ctrl_space_box, value);
    else if (strcmp(key, "decorations") == 0 && decor_combo) combobox_select(decor_combo, value == 2 ? 1 : 0);
    else if (strcmp(key, "verbose") == 0) set_box(verbose_box, value);
    else if (strcmp(key, "desktop_color") == 0 && color_button) colorbutton_set(color_button, (uint32_t)value);
    applying = 0;
}

void live_method(uint32_t index, const char *name, const char *title, uint32_t current)
{
    if (npending == MAX_METHODS)
        return;
    snprintf(pending_names[npending], sizeof pending_names[npending], "%s", title[0] ? title : name);
    if (current)
        pending_current = (int)index;
    npending++;
}

/* The combo box is filled again only when the methods changed. */
void live_methods_done(void)
{
    int same = npending == nmethods;
    for (int i = 0; same && i < nmethods; i++)
        same = strcmp(method_names[i], pending_names[i]) == 0;
    applying = 1;
    if (!same && method_combo) {
        memcpy(method_names, pending_names, sizeof method_names);
        nmethods = npending;
        combobox_clear(method_combo);
        for (int i = 0; i < nmethods; i++)
            combobox_add(method_combo, method_names[i]);
    }
    if (method_combo && pending_current != current_method) {
        current_method = pending_current;
        combobox_select(method_combo, current_method);
    } else if (method_combo && !same) {
        combobox_select(method_combo, current_method);
    }
    applying = 0;
    npending = 0;
}

void live_tick(void)
{
    debug_get_input_methods(debug_proxy);
}

/* ---- controls ---- */

static void set(const char *key, int value)
{
    if (!applying)
        setting_send(key, value);
}

static int on_apply_mode(struct widget *w, void *args, void *arg)
{
    if (res_combo->value < 0 || res_combo->value >= nres)
        return 1;
    char text[32];
    snprintf(text, sizeof text, "%s@%d", resolutions[res_combo->value], scale_combo->value == 1 ? 2 : 1);
    int mode = display_mode_parse(text);
    if (mode)
        set("display_mode", mode);
    return 1;
}
static int on_follow(struct widget *w, void *args, void *arg) { set("display_follow", w->value); return 0; }
static int on_frame(struct widget *w, void *args, void *arg) { set("frame_ms", w->value); return 0; }
static int on_rate(struct widget *w, void *args, void *arg) { set("repeat_rate", w->value); return 0; }
static int on_delay(struct widget *w, void *args, void *arg) { set("repeat_delay", w->value); return 0; }
static int on_keymap(struct widget *w, void *args, void *arg) { set("keymap_reload", 1); return 0; }
static int on_speed(struct widget *w, void *args, void *arg) { set("pointer_speed", w->value); return 0; }
static int on_accel(struct widget *w, void *args, void *arg) { set("pointer_accel", w->value); return 0; }
static int on_method(struct widget *w, void *args, void *arg)
{
    if (!applying && w->value >= 0) {
        current_method = w->value;
        set("input_method", w->value);
    }
    return 0;
}
static int on_shift(struct widget *w, void *args, void *arg) { set("ime_shift_toggle", w->value); return 0; }
static int on_ctrl_space(struct widget *w, void *args, void *arg) { set("ime_ctrl_space", w->value); return 0; }
static int on_decor(struct widget *w, void *args, void *arg) { set("decorations", w->value == 1 ? 2 : 1); return 0; }
static int on_color(struct widget *w, void *args, void *arg)
{
    set("desktop_color", ((struct sig_change *)args)->value);
    return 0;
}
static int on_verbose(struct widget *w, void *args, void *arg) { set("verbose", w->value); return 0; }

/* ---- the page ---- */

static int row;

static void heading(struct widget *grid, const char *text)
{
    if (row)
        widget_set_grid(separator_new(grid), row++, 0, 1, 2);
    widget_set_grid(label_new(grid, text), row++, 0, 1, 2);
}

static void field(struct widget *grid, const char *name, struct widget *control)
{
    struct widget *l = label_new(grid, name);
    widget_set_hint(l, 190, 0);
    widget_set_grid(l, row, 0, 1, 1);
    widget_set_grid(control, row++, 1, 1, 1);
}

static void full_row(struct widget *grid, struct widget *control)
{
    widget_set_grid(control, row++, 1, 1, 1);
}

void live_build(struct widget *tabs)
{
    struct widget *page = tabs_add(tabs, "Settings");
    struct widget *scroll = scrollarea_new(page);
    widget_set_stretch(scroll, 1, 1);
    struct widget *grid = grid_new((struct widget *)scroll->user);
    widget_set_stretch(grid, 1, 0);
    grid_set_stretch(grid, -1, 1, 1);
    row = 0;

    heading(grid, "Display");
    mode_label = label_new(grid, "");
    field(grid, "Current mode", mode_label);
    struct widget *line = box_new(grid, 0);
    res_combo = combobox_new(line);
    nres = display_resolutions(&resolutions);
    for (int i = 0; i < nres; i++)
        combobox_add(res_combo, resolutions[i]);
    widget_set_stretch(res_combo, 1, 0);
    scale_combo = combobox_new(line);
    combobox_add(scale_combo, "scale 1");
    combobox_add(scale_combo, "scale 2");
    widget_connect(button_new(line, "Apply"), "clicked", on_apply_mode, NULL);
    field(grid, "Mode", line);
    follow_box = checkbox_new(grid, "Follow the size of the host window");
    widget_connect(follow_box, "toggled", on_follow, NULL);
    full_row(grid, follow_box);
    frame_spin = spinner_new(grid, 4, 200, 16);
    widget_connect(frame_spin, "changed", on_frame, NULL);
    field(grid, "Frame interval (ms)", frame_spin);

    heading(grid, "Keyboard");
    rate_spin = spinner_new(grid, 1, 100, 30);
    widget_connect(rate_spin, "changed", on_rate, NULL);
    field(grid, "Repeat rate (per second)", rate_spin);
    delay_spin = spinner_new(grid, 50, 2000, 500);
    widget_connect(delay_spin, "changed", on_delay, NULL);
    field(grid, "Repeat delay (ms)", delay_spin);
    struct widget *reload = button_new(grid, "Reload the keymap");
    widget_connect(reload, "clicked", on_keymap, NULL);
    field(grid, "Keyboard layout", reload);

    heading(grid, "Pointer");
    speed_slider = slider_new(grid, -100, 100, 0);
    widget_connect(speed_slider, "changed", on_speed, NULL);
    field(grid, "Speed", speed_slider);
    accel_combo = combobox_new(grid);
    combobox_add(accel_combo, "flat");
    combobox_add(accel_combo, "adaptive");
    widget_connect(accel_combo, "changed", on_accel, NULL);
    field(grid, "Acceleration", accel_combo);

    heading(grid, "Input method");
    method_combo = combobox_new(grid);
    widget_connect(method_combo, "changed", on_method, NULL);
    field(grid, "Method", method_combo);
    shift_box = checkbox_new(grid, "A Shift tap switches the input method");
    widget_connect(shift_box, "toggled", on_shift, NULL);
    full_row(grid, shift_box);
    ctrl_space_box = checkbox_new(grid, "Ctrl+Space and Super+Space switch the input method");
    widget_connect(ctrl_space_box, "toggled", on_ctrl_space, NULL);
    full_row(grid, ctrl_space_box);

    heading(grid, "Windows");
    decor_combo = combobox_new(grid);
    combobox_add(decor_combo, "drawn by X12 (server side)");
    combobox_add(decor_combo, "drawn by the application (client side)");
    widget_connect(decor_combo, "changed", on_decor, NULL);
    field(grid, "Decorations", decor_combo);
    color_button = colorbutton_new(grid, 0x00306080, "Desktop colour");
    widget_connect(color_button, "changed", on_color, NULL);
    field(grid, "Desktop colour", color_button);

    heading(grid, "Logging");
    verbose_box = checkbox_new(grid, "Log every frame and event on the console");
    widget_connect(verbose_box, "toggled", on_verbose, NULL);
    full_row(grid, verbose_box);
}
