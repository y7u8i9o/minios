/* desktop: the background layer surface. It draws the wallpaper, shows
 * the entries of /home/desktop as icons, opens them by their MIME type
 * on a double click, offers context menus, and applies
 * /etc/desktop.conf (wallpaper and compositor settings), re-reading the
 * file when its contents change. */
#include <gui/app.h>
#include <gui/mime.h>
#include <debug-client.h>
#include <dirent.h>
#include <stdarg.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define DESKTOP_DIR "/home/desktop"
#define CONF_PATH "/etc/desktop.conf"
#define CELL_W 90
#define CELL_H 84
#define ICON_SIZE 32
#define MAX_ENTRIES 64

enum { MODE_FILL, MODE_CENTER, MODE_TILE, MODE_STRETCH };

struct entry {
    char name[NAME_MAX + 1];
    char label[NAME_MAX + 1];
    int dir;
    const struct image *icon;
};

struct conf {
    char wallpaper[128];
    int display_mode;               /* packed WxH@S, 0 when the file has none */
    int mode;
    uint32_t color;
    int repeat_rate, repeat_delay;
    int frame_ms;                   /* 0 when the file has none */
    int decorations;                /* 0 none, 1 server, 2 client */
    char keymap[32];
};

static struct app *app;
static struct widget *win, *desk, *item_menu, *desk_menu;
static struct wire_proxy *settings;
static struct entry entries[MAX_ENTRIES];
static int nentries, selected = -1;
static struct conf conf = { "", MODE_FILL, 0x00306080, 30, 500 };  /* solid colour by default */
static char conf_text[1024];
static struct image *wallpaper;
static struct surface bg;       /* wallpaper scaled to the window */
static long last_click_ms;
static int last_click_entry = -1;

static pid_t spawn(const char *prog, const char *arg)
{
    pid_t pid = fork();
    if (pid == 0) {
        char *const args[] = { (char *)prog, (char *)arg, NULL };
        execv(prog, args);
        _exit(127);
    }
    return pid;
}

static void logline(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    printf("desktop: ");
    vprintf(fmt, ap);
    printf("\n");
    va_end(ap);
    fflush(stdout);
}

/* ---- icons ---- */

static struct { const char *name; struct image *img; } icons[8];

/* Icons are 16x16; the desktop shows them doubled. */
static const struct image *icon_big(const char *name)
{
    for (int i = 0; i < 8; i++)
        if (icons[i].name && strcmp(icons[i].name, name) == 0)
            return icons[i].img;
    const struct image *src = icon_get(name);
    if (!src)
        return NULL;
    struct image *big = malloc(sizeof *big);
    if (!big)
        return NULL;
    big->w = src->w * 2;
    big->h = src->h * 2;
    big->pixels = malloc((size_t)big->w * big->h * 4);
    for (int y = 0; y < big->h; y++)
        for (int x = 0; x < big->w; x++)
            big->pixels[y * big->w + x] = src->pixels[(y / 2) * src->w + x / 2];
    for (int i = 0; i < 8; i++)
        if (!icons[i].name) {
            icons[i].name = name;
            icons[i].img = big;
            break;
        }
    return big;
}

/* ---- entries ---- */

static int entry_cmp(const void *a, const void *b)
{
    const struct entry *x = a, *y = b;
    if (x->dir != y->dir)
        return y->dir - x->dir;
    return strcmp(x->name, y->name);
}

static void refresh(void)
{
    nentries = 0;
    DIR *d = opendir(DESKTOP_DIR);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d)) != NULL && nentries < MAX_ENTRIES) {
            if (e->d_name[0] == '.')
                continue;
            struct entry *en = &entries[nentries++];
            strlcpy(en->name, e->d_name, sizeof en->name);
            en->dir = e->d_type == DT_DIR;
            const char *type = mime_type(en->name, en->dir);
            en->icon = icon_big(mime_icon(type));
            strlcpy(en->label, en->name, sizeof en->label);
            size_t n = strlen(en->label);
            if (strcmp(type, MIME_LAUNCHER) == 0 && n > 4)
                en->label[n - 4] = '\0';
        }
        closedir(d);
    }
    qsort(entries, (size_t)nentries, sizeof entries[0], entry_cmp);
    if (selected >= nentries)
        selected = -1;
    if (desk)
        widget_invalidate(desk);
}

static void entry_path(int i, char *buf, size_t size)
{
    snprintf(buf, size, DESKTOP_DIR "/%s", entries[i].name);
}

static int rows_per_column(int h)
{
    int rows = (h - 20) / CELL_H;
    return rows < 1 ? 1 : rows;
}

static void cell_of(int i, int h, int *x, int *y)
{
    int rows = rows_per_column(h);
    *x = 10 + (i / rows) * CELL_W;
    *y = 10 + (i % rows) * CELL_H;
}

static int entry_at(int px, int py, int h)
{
    for (int i = 0; i < nentries; i++) {
        int x, y;
        cell_of(i, h, &x, &y);
        if (px >= x && px < x + CELL_W && py >= y && py < y + CELL_H)
            return i;
    }
    return -1;
}

static void open_entry(int i)
{
    char path[512];
    entry_path(i, path, sizeof path);
    logline("open %s", path);
    pid_t pid = entries[i].dir ? spawn("/bin/files", path) : mime_open(path);
    if (pid < 0)
        logline("cannot open %s: %s", path, strerror((int)-pid));
}

/* ---- wallpaper ---- */

static void scale_wallpaper(int w, int h)
{
    if (bg.width == w && bg.height == h && bg.pixels)
        return;
    free(bg.pixels);
    bg.pixels = calloc((size_t)w * h, 4);
    bg.width = w;
    bg.height = h;
    bg.stride = w;
    if (!bg.pixels)
        return;
    for (int i = 0; i < w * h; i++)
        bg.pixels[i] = conf.color;
    if (!wallpaper)
        return;
    const struct image *im = wallpaper;
    int dw = w, dh = h, dx = 0, dy = 0;
    switch (conf.mode) {
    case MODE_FILL: {
        /* Scale to cover the window, keeping the aspect ratio (16.16). */
        long sx = ((long)w << 16) / im->w, sy = ((long)h << 16) / im->h;
        long s = sx > sy ? sx : sy;
        dw = (int)((im->w * s) >> 16);
        dh = (int)((im->h * s) >> 16);
        dx = (w - dw) / 2;
        dy = (h - dh) / 2;
        break;
    }
    case MODE_CENTER:
        dw = im->w; dh = im->h; dx = (w - dw) / 2; dy = (h - dh) / 2;
        break;
    case MODE_TILE:
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++)
                bg.pixels[y * w + x] = im->pixels[(y % im->h) * im->w + x % im->w];
        return;
    case MODE_STRETCH:
        break;
    }
    for (int y = 0; y < h; y++) {
        int sy = (int)((long)(y - dy) * im->h / dh);
        if (y < dy || sy < 0 || sy >= im->h)
            continue;
        for (int x = 0; x < w; x++) {
            int sx = (int)((long)(x - dx) * im->w / dw);
            if (x < dx || sx < 0 || sx >= im->w)
                continue;
            bg.pixels[y * w + x] = im->pixels[sy * im->w + sx];
        }
    }
}

static void load_wallpaper(void)
{
    image_free(wallpaper);
    wallpaper = conf.wallpaper[0] ? image_load(conf.wallpaper) : NULL;
    if (conf.wallpaper[0] && !wallpaper)
        logline("cannot load wallpaper %s", conf.wallpaper);
    else
        logline("wallpaper %s mode %d", wallpaper ? conf.wallpaper : "none", conf.mode);
    bg.width = 0;               /* rescale on the next paint */
    if (desk)
        widget_invalidate(desk);
}

/* ---- configuration ---- */

static int mode_of(const char *s)
{
    if (strcmp(s, "center") == 0) return MODE_CENTER;
    if (strcmp(s, "tile") == 0) return MODE_TILE;
    if (strcmp(s, "stretch") == 0) return MODE_STRETCH;
    return MODE_FILL;
}

static int read_conf(char *buf, size_t size)
{
    int fd = open(CONF_PATH, O_RDONLY);
    if (fd < 0)
        return -errno;
    long n = read(fd, buf, size - 1);
    close(fd);
    if (n < 0)
        return (int)n;
    buf[n] = '\0';
    return 0;
}

/* "WxH" or "WxH@S" packed as the compositor's display_mode setting:
 * scale in bits 28..30, width in 14..27, height in 0..13. */
static int parse_display_mode(const char *v)
{
    char *end;
    long w = strtol(v, &end, 10);
    if (*end != 'x')
        return 0;
    long h = strtol(end + 1, &end, 10);
    long s = *end == '@' ? strtol(end + 1, NULL, 10) : 1;
    if (w < 640 || h < 480 || w > 8192 || h > 8192 || s < 1 || s > 4)
        return 0;
    return (int)((s << 28) | (w << 14) | h);
}

static void apply_conf(int first)
{
    struct conf c = conf;
    char text[sizeof conf_text];
    strlcpy(text, conf_text, sizeof text);
    for (char *line = strtok(text, "\n"); line; line = strtok(NULL, "\n")) {
        char *eq = strchr(line, '=');
        if (*line == '#' || !eq)
            continue;
        *eq = '\0';
        const char *v = eq + 1;
        if (strcmp(line, "wallpaper") == 0) strlcpy(c.wallpaper, v, sizeof c.wallpaper);
        else if (strcmp(line, "wallpaper_mode") == 0) c.mode = mode_of(v);
        else if (strcmp(line, "desktop_color") == 0) c.color = (uint32_t)strtoul(v, NULL, 0) & 0xffffff;
        else if (strcmp(line, "repeat_rate") == 0) c.repeat_rate = atoi(v);
        else if (strcmp(line, "repeat_delay") == 0) c.repeat_delay = atoi(v);
        else if (strcmp(line, "display_mode") == 0) c.display_mode = parse_display_mode(v);
        else if (strcmp(line, "frame_ms") == 0) c.frame_ms = atoi(v);
        else if (strcmp(line, "decorations") == 0) c.decorations = strcmp(v, "server") == 0 ? 1 : strcmp(v, "client") == 0 ? 2 : 0;
        else if (strcmp(line, "keymap") == 0) strlcpy(c.keymap, v, sizeof c.keymap);
    }
    int wall_changed = first || strcmp(c.wallpaper, conf.wallpaper) != 0 || c.mode != conf.mode || c.color != conf.color;
    if (settings && (first || c.color != conf.color))
        settings_set(settings, "desktop_color", (int32_t)c.color);
    if (settings && (first || c.repeat_rate != conf.repeat_rate))
        settings_set(settings, "repeat_rate", c.repeat_rate);
    if (settings && (first || c.repeat_delay != conf.repeat_delay))
        settings_set(settings, "repeat_delay", c.repeat_delay);
    if (settings && c.display_mode && (first || c.display_mode != conf.display_mode))
        settings_set(settings, "display_mode", c.display_mode);
    if (settings && c.frame_ms && (first || c.frame_ms != conf.frame_ms))
        settings_set(settings, "frame_ms", c.frame_ms);
    if (settings && c.decorations && (first || c.decorations != conf.decorations))
        settings_set(settings, "decorations", c.decorations);
    if (settings && c.keymap[0] && (first || strcmp(c.keymap, conf.keymap) != 0))
        settings_set(settings, "keymap_reload", 1);
    conf = c;
    if (wall_changed)
        load_wallpaper();
    gui_flush();
    logline("config applied");
}

static void poll_conf(void *arg)
{
    char text[sizeof conf_text];
    if (read_conf(text, sizeof text) < 0)
        return;
    if (strcmp(text, conf_text) != 0) {
        strlcpy(conf_text, text, sizeof conf_text);
        apply_conf(0);
    }
    /* Also pick up files added by other programs. */
    refresh();
    while (waitpid(-1, NULL, WNOHANG) > 0)
        ;
}

/* ---- the desktop widget ---- */

static void desk_paint(struct widget *w, struct painter *p)
{
    scale_wallpaper(w->w, w->h);
    if (bg.pixels)
        painter_blit(p, 0, 0, &bg);
    else
        painter_fill(p, 0, 0, w->w, w->h, conf.color);
    for (int i = 0; i < nentries; i++) {
        int x, y;
        cell_of(i, w->h, &x, &y);
        if (i == selected)
            painter_rounded(p, x + 2, y + 2, CELL_W - 4, CELL_H - 4, 0x00405870, 0x00c0d0e0);
        if (entries[i].icon)
            painter_image(p, x + (CELL_W - entries[i].icon->w) / 2, y + 8, entries[i].icon);
        const char *label = entries[i].label;
        int tw = painter_text_width(p, label, -1);
        if (tw > CELL_W - 6) tw = CELL_W - 6;
        int tx = x + (CELL_W - tw) / 2;
        painter_push(p, x + 3, y + 8 + ICON_SIZE + 4, CELL_W - 6, painter_text_height(p) + 2);
        painter_text(p, tx - (x + 3) + 1, 1, label, 0x00000000);
        painter_text(p, tx - (x + 3), 0, label, 0x00ffffff);
        painter_pop(p);
    }
}

static void select_entry(int i)
{
    if (i != selected) {
        selected = i;
        widget_invalidate(desk);
    }
}

static int desk_event(struct widget *w, struct event *e)
{
    switch (e->type) {
    case EV_MOUSE_DOWN: {
        int i = entry_at(e->x, e->y, w->h);
        widget_focus(w);
        if (e->button & 1) {
            select_entry(i);
            long now = uptime_ms();
            if (i >= 0 && i == last_click_entry && now - last_click_ms < 500) {
                last_click_entry = -1;
                open_entry(i);
            } else {
                last_click_entry = i;
                last_click_ms = now;
            }
            return 1;
        }
        if (e->button & 2) {
            select_entry(i);
            logline("menu %s", i >= 0 ? entries[i].name : "desktop");
            menu_popup(i >= 0 ? item_menu : desk_menu, e->x, e->y);
            return 1;
        }
        return 0;
    }
    case EV_KEY_DOWN:
        if (e->ch == '\n' && selected >= 0) { open_entry(selected); return 1; }
        if (e->code == 0x3f) { refresh(); return 1; }                   /* F5 */
        if (e->code == 0xd3 && selected >= 0) {                          /* Delete */
            struct sig_click c = { 1, 0, 0 };
            widget_emit(widget_find(item_menu, "delete"), "clicked", &c);
            return 1;
        }
        return 0;
    default:
        return 0;
    }
}

static const struct widget_class desk_class = { "desktop", sizeof(struct widget), NULL, NULL, desk_paint, desk_event, NULL };

/* ---- menu actions ---- */

static int on_open(struct widget *w, void *args, void *arg)
{
    if (selected >= 0)
        open_entry(selected);
    return 1;
}
static int on_open_with(struct widget *w, void *args, void *arg)
{
    if (selected < 0)
        return 1;
    char prog[128] = "/bin/";
    if (!app_prompt(app, "Open with", "Program:", prog, sizeof prog))
        return 1;
    char path[512];
    entry_path(selected, path, sizeof path);
    logline("open %s with %s", path, prog);
    spawn(prog, path);
    return 1;
}
static int on_rename(struct widget *w, void *args, void *arg)
{
    if (selected < 0)
        return 1;
    char name[NAME_MAX + 1];
    strlcpy(name, entries[selected].name, sizeof name);
    if (!app_prompt(app, "Rename", "New name:", name, sizeof name) || !name[0] || strchr(name, '/'))
        return 1;
    char from[512], to[512];
    entry_path(selected, from, sizeof from);
    snprintf(to, sizeof to, DESKTOP_DIR "/%s", name);
    if (rename(from, to) < 0)
        logline("rename failed: %s", strerror(errno));
    refresh();
    return 1;
}
static int on_delete(struct widget *w, void *args, void *arg)
{
    if (selected < 0)
        return 1;
    static const char *const buttons[] = { "Delete", "Cancel" };
    char text[300];
    snprintf(text, sizeof text, "Delete \"%s\"?", entries[selected].name);
    if (app_dialog(app, "Delete", text, buttons, 2) != 0)
        return 1;
    char path[512];
    entry_path(selected, path, sizeof path);
    int r = entries[selected].dir ? rmdir(path) : unlink(path);
    if (r < 0)
        logline("delete failed: %s", strerror(errno));
    else
        logline("deleted %s", path);
    selected = -1;
    refresh();
    return 1;
}
static int on_new_folder(struct widget *w, void *args, void *arg)
{
    char name[NAME_MAX + 1] = "New folder";
    if (!app_prompt(app, "New folder", "Name:", name, sizeof name) || !name[0] || strchr(name, '/'))
        return 1;
    char path[512];
    snprintf(path, sizeof path, DESKTOP_DIR "/%s", name);
    if (mkdir(path, 0755) < 0)
        logline("mkdir failed: %s", strerror(errno));
    refresh();
    return 1;
}
static int on_new_file(struct widget *w, void *args, void *arg)
{
    char name[NAME_MAX + 1] = "New file.txt";
    if (!app_prompt(app, "New text file", "Name:", name, sizeof name) || !name[0] || strchr(name, '/'))
        return 1;
    char path[512];
    snprintf(path, sizeof path, DESKTOP_DIR "/%s", name);
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        logline("create failed: %s", strerror(errno));
    else
        close(fd);
    refresh();
    return 1;
}
static int on_refresh(struct widget *w, void *args, void *arg) { refresh(); return 1; }
static int on_settings(struct widget *w, void *args, void *arg)
{
    spawn("/bin/settings", arg);
    return 1;
}

static void build_menus(void)
{
    item_menu = popupmenu_new(win);
    widget_connect(menu_add(item_menu, "Open", "open"), "clicked", on_open, NULL);
    widget_connect(menu_add(item_menu, "Open with...", NULL), "clicked", on_open_with, NULL);
    menu_add_separator(item_menu);
    widget_connect(menu_add(item_menu, "Rename...", "edit"), "clicked", on_rename, NULL);
    struct widget *del = menu_add(item_menu, "Delete", "quit");
    widget_set_id(del, "delete");
    widget_connect(del, "clicked", on_delete, NULL);
    desk_menu = popupmenu_new(win);
    widget_connect(menu_add(desk_menu, "New folder...", "folder"), "clicked", on_new_folder, NULL);
    widget_connect(menu_add(desk_menu, "New text file...", "new"), "clicked", on_new_file, NULL);
    menu_add_separator(desk_menu);
    widget_connect(menu_add(desk_menu, "Refresh", NULL), "clicked", on_refresh, NULL);
    menu_add_separator(desk_menu);
    widget_connect(menu_add(desk_menu, "Change wallpaper...", "paint"), "clicked", on_settings, "appearance");
    widget_connect(menu_add(desk_menu, "Settings", NULL), "clicked", on_settings, NULL);
}

int main(int argc, char **argv)
{
    app = app_create();
    if (!app)
        return 1;
    settings = gui_bind_global("settings", &settings_interface, 1);
    win = app_layer_window(app, 0, 0, 0, GUI_ANCHOR_TOP | GUI_ANCHOR_BOTTOM | GUI_ANCHOR_LEFT | GUI_ANCHOR_RIGHT,
                           0, 1, "desktop");
    if (!win)
        return 1;
    widget_set_padding(win, 0);
    desk = widget_new(&desk_class, win);
    desk->focusable = 1;
    widget_set_stretch(desk, 1, 1);
    build_menus();
    mkdir(DESKTOP_DIR, 0755);
    if (read_conf(conf_text, sizeof conf_text) < 0)
        logline("no " CONF_PATH ", using defaults");
    apply_conf(1);
    refresh();
    logline("started with %d entries", nentries);
    app_timer_add(app, 1000, 1, poll_conf, NULL);
    app_run(app);
    app_destroy(app);
    return 0;
}
