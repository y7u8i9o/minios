/* settings: the user's settings. Appearance and keyboard values are
 * stored in /etc/desktop.conf, which the desktop client applies within
 * a second; file type handlers are stored in /etc/mime.apps.
 * "settings set KEY VALUE" changes one desktop.conf entry and exits. */
#include <gui/app.h>
#include <gui/mime.h>
#include <gui/model.h>
#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/utsname.h>
#include <unistd.h>

#define CONF_PATH "/etc/desktop.conf"
#define WALLPAPER_DIR "/usr/share/wallpapers"

static const char *const keys[] = { "wallpaper", "wallpaper_mode", "desktop_color", "repeat_rate", "repeat_delay" };
#define NKEYS 5
static char values[NKEYS][128] = { "/usr/share/wallpapers/default.png", "fill", "0x306080", "30", "500" };
static const char *const modes[] = { "fill", "center", "tile", "stretch" };

static struct app *app;
static struct widget *wall_combo, *mode_combo, *red, *green, *blue, *swatch, *rate_spin, *delay_spin;
static struct widget *apps_table, *prog_field, *type_label;
static char wallpapers[16][128];
static int nwallpapers;
static int selected_type = -1;

/* ---- the configuration file ---- */

static void conf_read(void)
{
    FILE *f = fopen(CONF_PATH, "r");
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
        for (int i = 0; i < NKEYS; i++)
            if (strcmp(line, keys[i]) == 0)
                strlcpy(values[i], eq + 1, sizeof values[i]);
    }
    fclose(f);
}

static int conf_write(void)
{
    FILE *f = fopen(CONF_PATH, "w");
    if (!f)
        return -1;
    fprintf(f, "# desktop settings, edited by /bin/settings\n");
    for (int i = 0; i < NKEYS; i++)
        fprintf(f, "%s=%s\n", keys[i], values[i]);
    fclose(f);
    return 0;
}

static int key_index(const char *key)
{
    for (int i = 0; i < NKEYS; i++)
        if (strcmp(key, keys[i]) == 0)
            return i;
    return -1;
}

/* ---- appearance ---- */

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

static int on_swatch(struct widget *w, void *args, void *arg)
{
    struct painter *p = ((struct sig_paint *)args)->p;
    painter_fill(p, 0, 0, w->w, w->h, (uint32_t)(red->value << 16 | green->value << 8 | blue->value));
    painter_frame(p, 0, 0, w->w, w->h, 0x00000000);
    return 1;
}
static int on_color(struct widget *w, void *args, void *arg) { widget_invalidate(swatch); return 1; }

static int on_apply(struct widget *w, void *args, void *arg)
{
    int wi = wall_combo->value, mi = mode_combo->value;
    if (wi >= 0 && wi < nwallpapers)
        strlcpy(values[0], wallpapers[wi], sizeof values[0]);
    else if (wi == nwallpapers)
        values[0][0] = '\0';
    strlcpy(values[1], modes[mi >= 0 && mi < 4 ? mi : 0], sizeof values[1]);
    snprintf(values[2], sizeof values[2], "0x%02x%02x%02x", red->value, green->value, blue->value);
    snprintf(values[3], sizeof values[3], "%d", rate_spin->value);
    snprintf(values[4], sizeof values[4], "%d", delay_spin->value);
    if (conf_write() < 0) {
        static const char *const buttons[] = { "OK" };
        app_dialog(app, "Settings", "The settings file cannot be written.", buttons, 1);
        return 1;
    }
    printf("settings: saved\n");
    fflush(stdout);
    return 1;
}

static void build_appearance(struct widget *page)
{
    struct widget *grid = grid_new(page);
    widget_set_stretch(grid, 1, 0);
    grid_set_stretch(grid, -1, 1, 1);
    int r = 0;
    widget_set_grid(label_new(grid, "Wallpaper"), r, 0, 1, 1);
    wall_combo = combobox_new(grid);
    list_wallpapers();
    int current = nwallpapers;
    for (int i = 0; i < nwallpapers; i++) {
        const char *base = strrchr(wallpapers[i], '/');
        combobox_add(wall_combo, base ? base + 1 : wallpapers[i]);
        if (strcmp(wallpapers[i], values[0]) == 0)
            current = i;
    }
    combobox_add(wall_combo, "(none)");
    combobox_select(wall_combo, current);
    widget_set_grid(wall_combo, r++, 1, 1, 1);
    widget_set_grid(label_new(grid, "Wallpaper mode"), r, 0, 1, 1);
    mode_combo = combobox_new(grid);
    int mode = 0;
    for (int i = 0; i < 4; i++) {
        combobox_add(mode_combo, modes[i]);
        if (strcmp(modes[i], values[1]) == 0)
            mode = i;
    }
    combobox_select(mode_combo, mode);
    widget_set_grid(mode_combo, r++, 1, 1, 1);
    uint32_t color = (uint32_t)strtoul(values[2], NULL, 0);
    struct widget **sliders[] = { &red, &green, &blue };
    const char *names[] = { "Desktop red", "Desktop green", "Desktop blue" };
    for (int i = 0; i < 3; i++) {
        widget_set_grid(label_new(grid, names[i]), r, 0, 1, 1);
        *sliders[i] = slider_new(grid, 0, 255, (int)(color >> (16 - 8 * i)) & 0xff);
        widget_set_grid(*sliders[i], r++, 1, 1, 1);
        widget_connect(*sliders[i], "changed", on_color, NULL);
    }
    widget_set_grid(label_new(grid, "Desktop colour"), r, 0, 1, 1);
    swatch = canvas_new(grid);
    widget_set_hint(swatch, 120, 24);
    widget_connect(swatch, "paint", on_swatch, NULL);
    widget_set_grid(swatch, r++, 1, 1, 1);
}

static void build_keyboard(struct widget *page)
{
    struct widget *grid = grid_new(page);
    widget_set_stretch(grid, 1, 0);
    grid_set_stretch(grid, -1, 1, 1);
    widget_set_grid(label_new(grid, "Key repeat rate (per s)"), 0, 0, 1, 1);
    rate_spin = spinner_new(grid, 1, 100, atoi(values[3]));
    widget_set_grid(rate_spin, 0, 1, 1, 1);
    widget_set_grid(label_new(grid, "Key repeat delay (ms)"), 1, 0, 1, 1);
    delay_spin = spinner_new(grid, 50, 2000, atoi(values[4]));
    widget_set_grid(delay_spin, 1, 1, 1, 1);
}

/* ---- file types ---- */

static int m_rows(struct model *m, int parent) { return parent < 0 ? mime_handler_count() : 0; }
static int m_child(struct model *m, int parent, int index) { return index; }
static int m_columns(struct model *m) { return 2; }
static const char *m_cell(struct model *m, int row, int col, char *buf, size_t size)
{
    return col == 0 ? mime_handler_type(row) : mime_handler_program(row);
}
static const char *m_header(struct model *m, int col) { return col == 0 ? "Type" : "Program"; }
static struct model apps_model = { m_rows, m_child, m_columns, m_cell, m_header, NULL, NULL };

static int on_type_selected(struct widget *w, void *args, void *arg)
{
    selected_type = ((struct sig_select *)args)->index;
    if (selected_type >= 0) {
        widget_set_text(type_label, mime_handler_type(selected_type));
        widget_set_text(prog_field, mime_handler_program(selected_type));
    }
    return 1;
}
static int on_set_program(struct widget *w, void *args, void *arg)
{
    if (selected_type < 0 || !widget_text(prog_field)[0])
        return 1;
    mime_set_handler(mime_handler_type(selected_type), widget_text(prog_field));
    if (mime_save(NULL) < 0)
        printf("settings: cannot save the file type table\n");
    else
        printf("settings: handler %s = %s\n", mime_handler_type(selected_type), widget_text(prog_field));
    fflush(stdout);
    view_refresh(apps_table);
    return 1;
}
static int on_add_type(struct widget *w, void *args, void *arg)
{
    char type[64] = "";
    if (!app_prompt(app, "New file type", "Type (for example text/x-log):", type, sizeof type) || !type[0])
        return 1;
    mime_set_handler(type, "/bin/gedit");
    mime_save(NULL);
    view_refresh(apps_table);
    return 1;
}

static void build_apps(struct widget *page)
{
    label_new(page, "Program opening each file type (type/* and * are fallbacks):");
    apps_table = table_new(page);
    view_set_model(apps_table, &apps_model);
    table_set_column_width(apps_table, 0, 200);
    table_set_column_width(apps_table, 1, 200);
    widget_set_stretch(apps_table, 1, 1);
    widget_connect(apps_table, "selected", on_type_selected, NULL);
    struct widget *row = box_new(page, 0);
    widget_set_stretch(row, 1, 0);
    type_label = label_new(row, "(select a type)");
    widget_set_hint(type_label, 150, 0);
    prog_field = textfield_new(row, "");
    widget_set_stretch(prog_field, 1, 0);
    widget_connect(prog_field, "activate", on_set_program, NULL);
    widget_connect(button_new(row, "Set"), "clicked", on_set_program, NULL);
    widget_connect(button_new(row, "Add type..."), "clicked", on_add_type, NULL);
}

/* ---- about ---- */

static void build_about(struct widget *page)
{
    struct utsname u;
    char text[256];
    if (uname(&u) == 0) {
        snprintf(text, sizeof text, "%s %s %s", u.sysname, u.release, u.machine);
        label_new(page, text);
    }
    int fd = open("/dev/meminfo", O_RDONLY);
    if (fd >= 0) {
        long n = read(fd, text, sizeof text - 1);
        close(fd);
        if (n > 0) {
            text[n] = '\0';
            for (char *line = strtok(text, "\n"); line; line = strtok(NULL, "\n"))
                label_new(page, line);
        }
    }
    snprintf(text, sizeof text, "Screen %dx%d", gui_screen_width(), gui_screen_height());
    label_new(page, text);
}

int main(int argc, char **argv)
{
    conf_read();
    if (argc == 4 && strcmp(argv[1], "set") == 0) {
        int i = key_index(argv[2]);
        if (i < 0) {
            fprintf(stderr, "settings: unknown key %s\n", argv[2]);
            return 1;
        }
        strlcpy(values[i], argv[3], sizeof values[i]);
        if (conf_write() < 0) {
            perror(CONF_PATH);
            return 1;
        }
        printf("settings: set %s %s\n", argv[2], argv[3]);
        return 0;
    }
    app = app_create();
    if (!app)
        return 1;
    struct widget *win = app_window(app, 520, 400, "settings");
    if (!win)
        return 1;
    struct widget *tabs = tabs_new(win);
    widget_set_stretch(tabs, 1, 1);
    build_appearance(tabs_add(tabs, "Appearance"));
    build_keyboard(tabs_add(tabs, "Keyboard"));
    build_apps(tabs_add(tabs, "File types"));
    build_about(tabs_add(tabs, "About"));
    if (argc > 1 && strcmp(argv[1], "appearance") == 0)
        tabs_select(tabs, 0);
    struct widget *bar = box_new(win, 0);
    widget_set_stretch(bar, 1, 0);
    struct widget *apply = button_new(bar, "Apply");
    widget_set_align(apply, ALIGN_END, ALIGN_CENTER);
    widget_set_stretch(apply, 1, 0);
    widget_connect(apply, "clicked", on_apply, NULL);
    app_run(app);
    app_destroy(app);
    return 0;
}
