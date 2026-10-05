/* settings: the user's settings in one window with a category list. Every
 * change is written to the user's configuration file immediately; the desktop client
 * notices the file within a second and applies it, pushing the values
 * X12 owns (colour, key repeat, display mode, frame interval, decorations,
 * keymap, pointer speed and acceleration) through the settings protocol. "settings set KEY VALUE" changes
 * one entry and exits; "settings PAGE" opens on that page. */
#include <minios/conf.h>
#include "settings.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct app *app;

/* ---- the configuration file ---- */

#define NKEYS 23
static const char *const keys[NKEYS] = {
    "wallpaper", "wallpaper_mode", "desktop_color", "repeat_rate", "repeat_delay", "display_mode",
    "frame_ms", "decorations", "keymap", "ui_font", "ui_font_px", "ui_scale", "term_font_px", "pointer_speed",
    "pointer_accel", "lang", "formats", "ime_engines", "ime_shift_toggle", "ime_ctrl_space", "ime_page_size",
    "ime_orientation", "display_follow",
};
static char values[NKEYS][128] = {
    "", "fill", "0x306080", "30", "500", "", "16", "client", "us", "DejaVu Sans", "14", "100", "13", "0", "adaptive",
    "", "", "pinyin,japanese", "1", "1", "5", "horizontal", "1",
};

static int key_index(const char *key)
{
    for (int i = 0; i < NKEYS; i++)
        if (strcmp(key, keys[i]) == 0)
            return i;
    return -1;
}

static void conf_read(void)
{
    char path[256];
    FILE *f = fopen(conf_read_path(path, sizeof path), "r");
    if (!f)
        return;
    char line[256];
    while (fgets(line, sizeof line, f)) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = '\0';
        char *eq = strchr(line, '=');
        if (line[0] == '#' || !eq)
            continue;
        *eq = '\0';
        int i = key_index(line);
        if (i >= 0)
            strlcpy(values[i], eq + 1, sizeof values[i]);
    }
    fclose(f);
}

static int conf_write(void)
{
    char path[256];
    FILE *f = fopen(conf_write_path(path, sizeof path), "w");
    if (!f)
        return -1;
    fprintf(f, "# desktop settings, edited by /bin/settings\n");
    for (int i = 0; i < NKEYS; i++)
        fprintf(f, "%s=%s\n", keys[i], values[i]);
    fclose(f);
    return 0;
}

const char *conf_get(const char *key)
{
    int i = key_index(key);
    return i < 0 ? "" : values[i];
}

int conf_int(const char *key, int fallback)
{
    const char *v = conf_get(key);
    return v[0] ? (int)strtol(v, NULL, 0) : fallback;
}

int conf_set(const char *key, const char *value)
{
    int i = key_index(key);
    if (i < 0)
        return -1;
    if (strcmp(values[i], value) == 0)
        return 0;
    strlcpy(values[i], value, sizeof values[i]);
    if (conf_write() < 0) {
        if (app) {
            const char *buttons[] = { _("OK") };
            app_dialog(app, _("Settings"), _("The settings file cannot be written."), buttons, 1);
        }
        return -1;
    }
    printf("settings: set %s %s\n", key, value);
    fflush(stdout);
    return 0;
}

int conf_set_int(const char *key, int value)
{
    char text[32];
    snprintf(text, sizeof text, "%d", value);
    return conf_set(key, text);
}

/* ---- pages ---- */

struct widget *row_label(struct widget *grid, int row, const char *text)
{
    struct widget *l = label_new(grid, text);
    widget_set_grid(l, row, 0, 1, 1);
    widget_set_hint(l, 150, 0);
    return l;
}

static int on_conf_checkbox(struct widget *w, void *args, void *key)
{
    conf_set(key, w->value ? "1" : "0");
    return 1;
}

struct widget *conf_checkbox_new(struct widget *parent, const char *text, const char *key, int fallback)
{
    struct widget *box = checkbox_new(parent, text);
    box->value = conf_int(key, fallback) != 0;
    widget_connect(box, "toggled", on_conf_checkbox, (void *)key);
    return box;
}

struct category {
    const char *name;
    const char *arg;            /* command line name */
    void (*build)(struct widget *page);
    struct widget *page;
};
static struct category categories[] = {
    { N_("Appearance"), "appearance", build_appearance, NULL },
    { N_("Display"), "display", build_display, NULL },
    { N_("Keyboard"), "keyboard", build_keyboard, NULL },
    { N_("Region and language"), "region", build_region, NULL },
    { N_("Mouse"), "mouse", build_mouse, NULL },
    { N_("Sound"), "sound", build_sound, NULL },
    { N_("Date and time"), "time", build_datetime, NULL },
    { N_("File types"), "filetypes", build_filetypes, NULL },
    { N_("Launcher"), "launcher", build_launcher, NULL },
    { N_("Users"), "users", build_users, NULL },
    { N_("System"), "system", build_system, NULL },
};
#define NCATEGORIES ((int)(sizeof categories / sizeof categories[0]))

static struct widget *sidebar, *pages;

static void show_page(int index)
{
    for (int i = 0; i < NCATEGORIES; i++)
        widget_set_visible(categories[i].page, i == index);
    widget_relayout(pages);
}

static int on_category(struct widget *w, void *args, void *arg)
{
    show_page(((struct sig_select *)args)->index);
    return 1;
}

int main(int argc, char **argv)
{
    conf_read();
    if (argc == 4 && strcmp(argv[1], "set") == 0) {
        if (key_index(argv[2]) < 0) {
            fprintf(stderr, "settings: unknown key %s\n", argv[2]);
            return 1;
        }
        app = NULL;
        return conf_set(argv[2], argv[3]) < 0 ? 1 : 0;
    }
    app = app_create();
    if (!app)
        return 1;
    textdomain("settings");
    struct widget *win = app_window(app, 680, 480, _("Settings"));
    if (!win)
        return 1;
    struct widget *split = box_new(win, 0);
    widget_set_stretch(split, 1, 1);
    sidebar = listview_new(split);
    widget_set_hint(sidebar, 170, 0);
    widget_set_stretch(sidebar, 0, 1);
    for (int i = 0; i < NCATEGORIES; i++)
        listview_add(sidebar, _(categories[i].name));
    widget_connect(sidebar, "selected", on_category, NULL);
    pages = box_new(split, 1);
    widget_set_stretch(pages, 1, 1);
    int start = 0;
    for (int i = 0; i < NCATEGORIES; i++) {
        categories[i].page = box_new(pages, 1);
        widget_set_stretch(categories[i].page, 1, 1);
        categories[i].build(categories[i].page);
        if (argc > 1 && strcmp(argv[1], categories[i].arg) == 0)
            start = i;
    }
    sidebar->value = start;
    show_page(start);
    widget_focus(sidebar);
    app_run(app);
    app_destroy(app);
    return 0;
}
