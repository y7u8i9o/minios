/* The launcher menu of the panel: a popup above the Menu button.
 *
 * The menu reads /etc/launcher and the table of installed packages
 * (LOCAL_LAUNCHER) each time it opens.  The entries of packages are
 * listed under the heading Applications in the order of their titles,
 * the entries of /etc/launcher under the heading System in the order of
 * the file.  The entry with the program @logout is drawn at the bottom
 * below a line.  Each entry has the icon /usr/share/icons/app-NAME.svg,
 * where NAME is the file name of its program, or app-default.svg.
 *
 * The first row of the menu is a search field.  Typed characters filter
 * the entries by title without regard to case, Backspace removes the
 * last character, Up and Down move the selection and Enter starts the
 * selected entry.  The compositor closes the menu on Escape.  The size of
 * the menu is computed for all entries when it opens and is not changed
 * by the filter.  The menu uses two columns when one column is taller
 * than the screen above the panel. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <gui/image.h>
#include <gui/keymap.h>
#include <minios/local.h>
#include "panel.h"

#define MAX_ENTRIES 64
#define MAX_ICONS 64

enum section { SEC_APPS, SEC_SYSTEM, SEC_LOGOUT };

struct entry {
    char title[32];
    char path[96];
    enum section section;
    const struct image *icon;
};

/* A row is a heading or an entry at a position in the menu. */
struct row {
    int entry;                      /* The row is a heading when entry is -1. */
    const char *heading;
    int x, y, w, h;
};

static struct entry entries[MAX_ENTRIES];
static int nentries;
static struct row rows[MAX_ENTRIES + 4];
static int nrows;
static int columns, menu_w, menu_h;
static char query[32];
static int selected = -1;           /* selected is an index into rows or -1. */
static struct canvas menu;
static struct wire_proxy *popup, *keyboard, *keyboard_surface;
static struct keymap *keymap;
static int modifiers;
static int open_state;

static struct { char name[40]; int scale; struct image *img; } icons[MAX_ICONS];
static int nicons;

/* icon_load returns the icon NAME rendered at the output scale.  A
 * missing icon is cached as NULL. */
static const struct image *icon_load(const char *name)
{
    int scale = output_scale > 0 ? output_scale : 1;
    for (int i = 0; i < nicons; i++)
        if (icons[i].scale == scale && strcmp(icons[i].name, name) == 0)
            return icons[i].img;
    if (nicons == MAX_ICONS)
        return NULL;
    char path[96];
    snprintf(path, sizeof path, "/usr/share/icons/%s.svg", name);
    struct image *img = image_load_svg(path, LAUNCHER_ICON * scale, MENU_ICON);
    if (img)
        img->scale = scale;
    strlcpy(icons[nicons].name, name, sizeof icons[nicons].name);
    icons[nicons].scale = scale;
    icons[nicons].img = img;
    nicons++;
    return img;
}

static const struct image *entry_icon(const char *path)
{
    char name[40];
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    if (base[0] == '@')
        base++;
    snprintf(name, sizeof name, "app-%s", base);
    const struct image *img = icon_load(name);
    return img ? img : icon_load("app-default");
}

static void load_file(const char *path, enum section section)
{
    FILE *f = fopen(path, "r");
    if (!f)
        return;
    char line[160];
    while (nentries < MAX_ENTRIES && fgets(line, sizeof line, f)) {
        char *eq = strchr(line, '='), *nl = strchr(line, '\n');
        if (nl)
            *nl = '\0';
        if (!eq || line[0] == '#')
            continue;
        *eq = '\0';
        struct entry *e = &entries[nentries++];
        strlcpy(e->title, line, sizeof e->title);
        strlcpy(e->path, eq + 1, sizeof e->path);
        e->section = strcmp(e->path, "@logout") == 0 ? SEC_LOGOUT : section;
        e->icon = entry_icon(e->path);
    }
    fclose(f);
}

static int compare_titles(const void *a, const void *b)
{
    const struct entry *x = a, *y = b;
    return strcasecmp(x->title, y->title);
}

/* load_entries reads both tables and sorts the entries of packages by
 * title.  The table of packages is read first, and its entries are at the
 * start of the array. */
static void load_entries(void)
{
    nentries = 0;
    load_file(LOCAL_LAUNCHER, SEC_APPS);
    int apps = nentries;
    qsort(entries, (size_t)apps, sizeof entries[0], compare_titles);
    load_file("/etc/launcher", SEC_SYSTEM);
}

static int matches(const struct entry *e)
{
    if (!query[0])
        return 1;
    size_t n = strlen(query);
    for (const char *s = e->title; *s; s++)
        if (strncasecmp(s, query, n) == 0)
            return 1;
    return 0;
}

static int count_section(enum section s, int filtered)
{
    int n = 0;
    for (int i = 0; i < nentries; i++)
        n += entries[i].section == s && (!filtered || matches(&entries[i]));
    return n;
}

static int section_height(enum section s, int filtered)
{
    int n = count_section(s, filtered);
    return n ? LAUNCHER_HEADING_H + n * MENU_ITEM_H : 0;
}

/* layout_size computes the size of the menu for all entries.  It is
 * called when the menu opens. */
static void layout_size(void)
{
    int apps = section_height(SEC_APPS, 0), system = section_height(SEC_SYSTEM, 0);
    int footer = count_section(SEC_LOGOUT, 0) ? LAUNCHER_RULE_H + MENU_ITEM_H : 0;
    int fixed = 2 * MENU_PAD + LAUNCHER_SEARCH_H + footer;
    int room = screen_h - PANEL_H - 8;
    columns = apps && system && fixed + apps + system > room ? 2 : 1;
    menu_w = columns * LAUNCHER_COLUMN_W + 2 * MENU_PAD;
    menu_h = fixed + (columns == 2 ? (apps > system ? apps : system) : apps + system);
    if (menu_h > room)
        menu_h = room;
}

static void add_section(enum section s, const char *heading, int x, int *y)
{
    if (!count_section(s, 1))
        return;
    rows[nrows++] = (struct row){ -1, heading, x, *y, LAUNCHER_COLUMN_W, LAUNCHER_HEADING_H };
    *y += LAUNCHER_HEADING_H;
    for (int i = 0; i < nentries; i++)
        if (entries[i].section == s && matches(&entries[i])) {
            rows[nrows++] = (struct row){ i, NULL, x, *y, LAUNCHER_COLUMN_W, MENU_ITEM_H };
            *y += MENU_ITEM_H;
        }
}

/* layout_rows places the headings and the entries that match the query
 * inside the size computed by layout_size. */
static void layout_rows(void)
{
    nrows = 0;
    int top = MENU_PAD + LAUNCHER_SEARCH_H, y = top;
    add_section(SEC_APPS, "Applications", MENU_PAD, &y);
    if (columns == 2)
        y = top;
    add_section(SEC_SYSTEM, "System", MENU_PAD + (columns == 2 ? LAUNCHER_COLUMN_W : 0), &y);
    for (int i = 0; i < nentries; i++)
        if (entries[i].section == SEC_LOGOUT)
            rows[nrows++] = (struct row){ i, NULL, MENU_PAD, menu_h - MENU_PAD - MENU_ITEM_H,
                                          menu_w - 2 * MENU_PAD, MENU_ITEM_H };
    selected = -1;
    if (query[0])
        for (int r = 0; r < nrows && selected < 0; r++)
            if (rows[r].entry >= 0 && entries[rows[r].entry].section != SEC_LOGOUT)
                selected = r;
}

static void draw(void)
{
    struct painter p;
    canvas_painter(&p, &menu);
    painter_fill(&p, 0, 0, menu_w, menu_h, MENU_BG);
    painter_frame(&p, 0, 0, menu_w, menu_h, MENU_BORDER);

    /* The search field is drawn in the first row. */
    int fx = MENU_PAD + 2, fy = MENU_PAD + 2, fw = menu_w - 2 * MENU_PAD - 4, fh = LAUNCHER_SEARCH_H - 8;
    painter_rounded(&p, fx, fy, fw, fh, MENU_FIELD, MENU_BORDER);
    const struct image *glass = icon_load("search");
    int tx = fx + 8;
    if (glass) {
        painter_image(&p, tx, fy + (fh - image_lh(glass)) / 2, glass);
        tx += image_lw(glass) + 6;
    }
    int th = painter_text_height(&p);
    if (query[0]) {
        painter_text(&p, tx, fy + (fh - th) / 2, query, MENU_TEXT);
        tx += painter_text_width(&p, query, -1);
    } else {
        painter_text(&p, tx, fy + (fh - th) / 2, "Search", MENU_TEXT_DIM);
    }
    if (keyboard_surface == menu.surface)
        painter_fill(&p, tx + 1, fy + (fh - th) / 2, 1, th, MENU_TEXT);

    for (int r = 0; r < nrows; r++) {
        struct row *row = &rows[r];
        if (row->y + row->h > menu_h - MENU_PAD && row->entry >= 0 && entries[row->entry].section != SEC_LOGOUT)
            continue;
        if (row->entry < 0) {
            painter_text(&p, row->x + 8, row->y + row->h - th - 2, row->heading, MENU_TEXT_DIM);
            continue;
        }
        const struct entry *e = &entries[row->entry];
        if (e->section == SEC_LOGOUT)
            painter_fill(&p, MENU_PAD + 4, row->y - LAUNCHER_RULE_H / 2 - 1, menu_w - 2 * MENU_PAD - 8, 1, MENU_BORDER);
        if (r == selected)
            painter_rounded(&p, row->x + 2, row->y, row->w - 4, row->h, MENU_HOVER, 0xffffffffu);
        int x = row->x + 8;
        if (e->icon)
            painter_image(&p, x, row->y + (row->h - image_lh(e->icon)) / 2, e->icon);
        x += LAUNCHER_ICON + 8;
        panel_label(&p, x - 6, row->y, row->w - (x - row->x) - 2, row->h, e->title, MENU_TEXT, 0);
    }
    canvas_commit(&menu);
}

static int row_at(int x, int y)
{
    for (int r = 0; r < nrows; r++)
        if (rows[r].entry >= 0 && x >= rows[r].x && x < rows[r].x + rows[r].w && y >= rows[r].y &&
            y < rows[r].y + rows[r].h)
            return r;
    return -1;
}

static void launch(const struct entry *e)
{
    if (strcmp(e->path, "@logout") == 0) {
        log_line("logout");
        exit(0);                        /* startgui ends the session when the panel exits */
    }
    log_line("launch %s", e->path);
    pid_t pid = fork();
    if (pid == 0) {
        char *const args[] = { (char *)e->path, NULL };
        execvp(e->path, args);
        _exit(127);
    }
}

/* The functions below create and remove the popup. */

static void teardown(void)
{
    if (!menu.surface)
        return;
    if (popup) {
        popup_destroy(popup);
        popup = NULL;
    }
    surface_attach(menu.surface, NULL, 0, 0);
    surface_commit(menu.surface);
    surface_destroy(menu.surface);
    canvas_release_buffer(&menu);
    memset(&menu, 0, sizeof menu);
    keyboard_surface = NULL;
}

static void on_popup_configure(void *user, struct wire_proxy *p, uint32_t serial, int32_t x, int32_t y, int32_t w, int32_t h)
{
    if (p != popup)
        return;
    popup_ack_configure(p, serial);
    popup_grab(popup, seat, press_serial);
    open_state = 1;
    draw();
    log_line("menu opened");
    draw_panel();
}

static void on_popup_done(void *user, struct wire_proxy *p)
{
    /* The compositor dismissed the menu.  The popup and its surface are
     * released, because the next launcher_show would otherwise ask for a
     * role on a surface that still has one, which is a protocol error. */
    open_state = 0;
    teardown();
    log_line("menu closed");
    draw_panel();
}

static const struct popup_listener popup_events = { on_popup_configure, on_popup_done };

static void show(void)
{
    load_entries();
    query[0] = '\0';
    layout_size();
    layout_rows();
    if (!menu.surface && canvas_create(&menu, menu_w, menu_h) < 0)
        return;
    struct wire_proxy *pos = shell_create_positioner(shell);
    positioner_set_size(pos, menu_w, menu_h);
    positioner_set_anchor_rect(pos, 4, 4, MENU_BTN_W, 1);
    positioner_set_anchor(pos, POS_TOP_LEFT);
    positioner_set_gravity(pos, POS_TOP_RIGHT);    /* The menu extends up and to the right. */
    popup = shell_get_popup(shell, menu.surface, panel.surface, pos);
    popup_add_listener(popup, &popup_events, NULL);
    positioner_destroy(pos);
    open_state = 1;                 /* The configure event completes the opening. */
    wire_display_flush(display);
    draw_panel();
}

static void hide(void)
{
    if (!open_state)
        return;
    open_state = 0;
    teardown();
    draw_panel();
}

int launcher_is_open(void)
{
    return open_state;
}

void launcher_toggle(void)
{
    if (open_state)
        hide();
    else
        show();
}

int launcher_is_surface(const struct wire_proxy *surface)
{
    return surface && surface == menu.surface;
}

void launcher_pointer_motion(int x, int y)
{
    if (!open_state)
        return;
    int r = row_at(x, y);
    if (r >= 0 && r != selected) {
        selected = r;
        draw();
    }
}

void launcher_pointer_button(uint32_t button, uint32_t state, int x, int y)
{
    if (button != 1 || state != 1)
        return;
    int r = row_at(x, y);
    if (r < 0)
        return;
    int e = rows[r].entry;
    hide();
    launch(&entries[e]);
}

/* The functions below receive the keyboard while the popup has the
 * keyboard focus. */

static void move_selection(int step)
{
    int r = selected;
    for (int i = 0; i < nrows; i++) {
        r = r < 0 ? (step > 0 ? 0 : nrows - 1) : (r + step + nrows) % nrows;
        if (rows[r].entry >= 0) {
            selected = r;
            return;
        }
    }
}

static void on_keymap(void *user, struct wire_proxy *k, uint32_t format, int fd, uint32_t size)
{
    keymap_free(keymap);
    keymap = keymap_from_fd(fd, size);
    close(fd);
}

static void on_kbd_enter(void *user, struct wire_proxy *k, uint32_t serial, struct wire_proxy *s, const struct wire_array *keys)
{
    keyboard_surface = s;
    if (launcher_is_surface(s))
        draw();
}

static void on_kbd_leave(void *user, struct wire_proxy *k, uint32_t serial, struct wire_proxy *s)
{
    if (keyboard_surface == s)
        keyboard_surface = NULL;
}

static void on_key(void *user, struct wire_proxy *k, uint32_t serial, uint32_t time, uint32_t key, uint32_t state)
{
    if (!state || !open_state || !launcher_is_surface(keyboard_surface))
        return;
    if (key == KEY_UP || key == KEY_DOWN) {
        move_selection(key == KEY_DOWN ? 1 : -1);
        draw();
        return;
    }
    if (key == KEY_ENTER || key == KEY_KPENTER) {
        if (selected >= 0 && rows[selected].entry >= 0) {
            int e = rows[selected].entry;
            hide();
            launch(&entries[e]);
        }
        return;
    }
    size_t n = strlen(query);
    if (key == KEY_BACKSPACE) {
        if (n == 0)
            return;
        query[n - 1] = '\0';
    } else {
        int ch = keymap ? keymap_translate(keymap, key, modifiers) : 0;
        if (ch < 32 || ch > 126 || (modifiers & (KEYMAP_MOD_CTRL | KEYMAP_MOD_ALT)) || n + 1 >= sizeof query)
            return;
        query[n] = (char)ch;
        query[n + 1] = '\0';
    }
    layout_rows();
    log_line("menu search '%s'", query);
    draw();
}

static void on_modifiers(void *user, struct wire_proxy *k, uint32_t serial, uint32_t dep, uint32_t lat, uint32_t lock, uint32_t group)
{
    modifiers = (int)dep;
}

static void on_repeat(void *user, struct wire_proxy *k, int32_t rate, int32_t delay) {}

static const struct keyboard_listener keyboard_events = { on_keymap, on_kbd_enter, on_kbd_leave, on_key, on_modifiers, on_repeat };

void launcher_init(void)
{
    keyboard = seat_get_keyboard(seat);
    keyboard_add_listener(keyboard, &keyboard_events, NULL);
}
