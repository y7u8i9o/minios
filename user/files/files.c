/* files: the file manager window. A places list on the left, a table
 * of the current directory (icon, name, size, type, modified) on the
 * right, a tool bar with history and the path, menus with the file
 * operations of fsops.c, a context menu and a status bar. The listing
 * follows changes made by other programs every two seconds.
 *
 *   files [directory] */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <time.h>
#include <sys/stat.h>
#include <gui/app.h>
#include <gui/i18n.h>
#include <gui/mime.h>
#include <gui/model.h>
#include "files.h"

#define HISTORY 32
#define TYPEAHEAD_MS 1000

struct entry {
    char name[NAME_MAX + 1];
    long size;
    int64_t mtime;
    int dir, exec, hidden;
    const char *desc;           /* type shown in the table */
    const struct image *icon;
};

static struct app *app;
static struct widget *win, *path_field, *places, *table, *status, *sel_label, *hidden_check;
static struct widget *item_menu, *dir_menu, *back_button, *forward_button, *up_button;
static char cwd[512] = "/";
static struct entry *entries;
static int nentries, nhidden;
static int sort_col, sort_desc, show_hidden;
static char history[HISTORY][512];
static int hist_len, hist_pos;
static char clip_path[512];
static int clip_cut;
static char typed[64];
static long typed_ms;

static const struct { const char *name, *path; } places_list[] = {
    { N_("Home"), "/home" }, { N_("Desktop"), "/home/desktop" }, { N_("Root"), "/" }, { N_("Programs"), "/bin" },
    { N_("Shared files"), "/usr/share" }, { N_("Fonts"), "/etc/fonts" }, { N_("Devices"), "/dev" },
};

/* ---- the listing ---- */

static const char *describe(const char *type, int exec)
{
    static const struct { const char *type, *desc; } names[] = {
        { MIME_DIRECTORY, N_("Folder") }, { "text/plain", N_("Text") }, { "text/x-csrc", N_("C source") },
        { "text/x-shellscript", N_("Shell script") }, { "image/png", N_("PNG image") },
        { "audio/x-wav", N_("WAV audio") }, { MIME_LAUNCHER, N_("Launcher") },
    };
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++)
        if (strcmp(names[i].type, type) == 0)
            return _(names[i].desc);
    if (strcmp(type, "application/octet-stream") == 0)
        return exec ? _("Program") : _("File");
    return type;
}

static int name_cmp(const char *a, const char *b)
{
    for (;; a++, b++) {
        int x = (unsigned char)*a, y = (unsigned char)*b;
        if (x >= 'A' && x <= 'Z') x += 32;
        if (y >= 'A' && y <= 'Z') y += 32;
        if (x != y || !x)
            return x - y;
    }
}

static int cmp(const void *pa, const void *pb)
{
    const struct entry *a = pa, *b = pb;
    if (a->dir != b->dir)
        return b->dir - a->dir;
    int r = 0;
    if (sort_col == 1)
        r = a->size < b->size ? -1 : a->size > b->size;
    else if (sort_col == 2)
        r = strcmp(a->desc, b->desc);
    else if (sort_col == 3)
        r = a->mtime < b->mtime ? -1 : a->mtime > b->mtime;
    if (r == 0)
        r = name_cmp(a->name, b->name);
    return sort_desc ? -r : r;
}

/* Read a directory into a sorted array; returns the count or -errno. */
static int scan(const char *dir, struct entry **out, int *hidden_count)
{
    DIR *d = opendir(dir);
    if (!d)
        return -errno;
    struct entry *list = NULL;
    int n = 0, cap = 0, hidden = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
            continue;
        int is_hidden = e->d_name[0] == '.';
        if (is_hidden)
            hidden++;
        if (is_hidden && !show_hidden)
            continue;
        if (n == cap) {
            cap = cap ? cap * 2 : 32;
            struct entry *nl = realloc(list, (size_t)cap * sizeof *list);
            if (!nl)
                break;
            list = nl;
        }
        struct entry *en = &list[n++];
        memset(en, 0, sizeof *en);
        strlcpy(en->name, e->d_name, sizeof en->name);
        en->hidden = is_hidden;
        char path[512];
        fs_join(path, sizeof path, dir, e->d_name);
        struct stat st;
        if (stat(path, &st) == 0) {
            en->dir = S_ISDIR(st.st_mode);
            en->size = (long)st.st_size;
            en->mtime = st.st_mtime;
            en->exec = (st.st_mode & 0111) != 0;
        } else {
            en->dir = e->d_type == DT_DIR;
        }
        const char *type = mime_type(en->name, en->dir);
        en->desc = describe(type, en->exec);
        en->icon = icon_get(mime_icon(type));
    }
    closedir(d);
    if (n)
        qsort(list, (size_t)n, sizeof *list, cmp);
    *out = list;
    *hidden_count = hidden;
    return n;
}

static int same_listing(const struct entry *a, int na, const struct entry *b, int nb)
{
    if (na != nb)
        return 0;
    for (int i = 0; i < na; i++)
        if (strcmp(a[i].name, b[i].name) != 0 || a[i].size != b[i].size || a[i].mtime != b[i].mtime ||
            a[i].dir != b[i].dir)
            return 0;
    return 1;
}

/* ---- model ---- */

static int m_rows(struct model *m, int parent) { return parent < 0 ? nentries : 0; }
static int m_child(struct model *m, int parent, int index) { return index; }
static int m_columns(struct model *m) { return 4; }
static const char *m_cell(struct model *m, int row, int col, char *buf, size_t size)
{
    struct entry *e = &entries[row];
    switch (col) {
    case 0: return e->name;
    case 1: return e->dir ? "" : fs_human_size(e->size, buf, size);
    case 2: return e->desc;
    default: {
        time_t t = (time_t)e->mtime;
        struct tm tm;
        localtime_r(&t, &tm);
        strftime(buf, size, "%x %H:%M", &tm);
        return buf;
    }
    }
}
static const char *m_header(struct model *m, int col)
{
    static const char *const names[] = { N_("Name"), N_("Size"), N_("Type"), N_("Modified") };
    return _(names[col]);
}
static void m_sort(struct model *m, int col, int desc)
{
    sort_col = col;
    sort_desc = desc;
    if (nentries)
        qsort(entries, (size_t)nentries, sizeof *entries, cmp);
}
static const struct image *m_icon(struct model *m, int row) { return entries[row].icon; }

static struct model model = { m_rows, m_child, m_columns, m_cell, m_header, m_sort, NULL, m_icon };

/* ---- the window state ---- */

static struct entry *selected_entry(void)
{
    int row = table->value;
    return row >= 0 && row < nentries ? &entries[row] : NULL;
}

/* The message names the failed operation as a whole sentence, such as
 * "Cannot read the folder", which the dialog completes with the error. */
static void fail(const char *message, int err)
{
    char text[256];
    snprintf(text, sizeof text, "%s: %s.", message, strerror(err < 0 ? -err : err));
    const char *const buttons[] = { _("OK") };
    app_dialog(app, _("Files"), text, buttons, 1);
}

static void log_line(const char *fmt, const char *a, const char *b)
{
    printf("files: ");
    printf(fmt, a, b);
    printf("\n");
    fflush(stdout);
}

static void update_selection_label(void)
{
    struct entry *e = selected_entry();
    char text[320], size[32];
    if (!e)
        text[0] = '\0';
    else if (e->dir)
        snprintf(text, sizeof text, "%s, %s", e->name, e->desc);
    else
        snprintf(text, sizeof text, "%s, %s, %s", e->name, fs_human_size(e->size, size, sizeof size), e->desc);
    widget_set_text(sel_label, text);
}

static void update_chrome(void)
{
    char title[300];
    snprintf(title, sizeof title, _("%s - Files"), fs_basename(cwd));
    gui_set_title(window_state_of(win)->win, title);
    widget_set_text(path_field, cwd);
    widget_set_enabled(back_button, hist_pos > 0);
    widget_set_enabled(forward_button, hist_pos < hist_len - 1);
    widget_set_enabled(up_button, strcmp(cwd, "/") != 0);
    char items[32], text[64];
    snprintf(items, sizeof items, ngettext("%d item", "%d items", nentries), nentries);
    if (nhidden && !show_hidden)
        snprintf(text, sizeof text, ngettext("%s, %d hidden", "%s, %d hidden", nhidden), items, nhidden);
    else
        strlcpy(text, items, sizeof text);
    widget_set_text(status, text);
    update_selection_label();
}

/* Reread the directory; the selection is kept by name. */
static void refresh(void)
{
    char keep[NAME_MAX + 1] = "";
    struct entry *e = selected_entry();
    if (e)
        strlcpy(keep, e->name, sizeof keep);
    struct entry *list = NULL;
    int hidden = 0;
    int n = scan(cwd, &list, &hidden);
    if (n < 0) {
        fail(_("Cannot read the folder"), n);
        n = 0;
    }
    free(entries);
    entries = list;
    nentries = n;
    nhidden = hidden;
    view_refresh(table);
    table->value = -1;
    for (int i = 0; keep[0] && i < nentries; i++)
        if (strcmp(entries[i].name, keep) == 0)
            view_select(table, i);
    update_chrome();
}

static void select_name(const char *name)
{
    for (int i = 0; i < nentries; i++)
        if (strcmp(entries[i].name, name) == 0) {
            view_select(table, i);
            return;
        }
}

static void navigate(const char *path, int record)
{
    char target[512];
    strlcpy(target, path, sizeof target);
    fs_normalize(target);
    struct stat st;
    if (stat(target, &st) < 0 || !S_ISDIR(st.st_mode)) {
        fail(_("Cannot open the folder"), stat(target, &st) < 0 ? errno : ENOTDIR);
        widget_set_text(path_field, cwd);
        return;
    }
    char previous[512];
    strlcpy(previous, cwd, sizeof previous);
    strlcpy(cwd, target, sizeof cwd);
    if (record && strcmp(previous, cwd) != 0) {
        if (hist_len == HISTORY) {
            memmove(history[0], history[1], (size_t)(HISTORY - 1) * sizeof history[0]);
            hist_len--;
            hist_pos--;
        }
        hist_len = hist_pos + 1;
        strlcpy(history[hist_len++], cwd, sizeof history[0]);
        hist_pos = hist_len - 1;
    }
    table->value = -1;
    refresh();
    /* Going up selects the folder just left. */
    if (strncmp(previous, cwd, strlen(cwd)) == 0 && strlen(previous) > strlen(cwd)) {
        const char *rest = previous + strlen(cwd);
        while (*rest == '/')
            rest++;
        char first[NAME_MAX + 1];
        strlcpy(first, rest, sizeof first);
        char *slash = strchr(first, '/');
        if (slash)
            *slash = '\0';
        select_name(first);
    }
    widget_focus(table);
    log_line("cd %s", cwd, NULL);
}

static void open_entry(struct entry *e)
{
    char path[512];
    fs_join(path, sizeof path, cwd, e->name);
    if (e->dir) {
        navigate(path, 1);
        return;
    }
    log_line("open %s", path, NULL);
    pid_t pid = mime_open(path);
    if (pid < 0)
        fail(_("Cannot open the file"), (int)pid);
}

/* ---- handlers: navigation ---- */

static int on_activate(struct widget *w, void *args, void *arg)
{
    struct entry *e = selected_entry();
    if (e)
        open_entry(e);
    return 1;
}

static int on_selected(struct widget *w, void *args, void *arg)
{
    update_selection_label();
    return 0;
}

static int on_up(struct widget *w, void *args, void *arg)
{
    char parent[512];
    fs_join(parent, sizeof parent, cwd, "..");
    navigate(parent, 1);
    return 1;
}

static int on_back(struct widget *w, void *args, void *arg)
{
    if (hist_pos > 0) {
        hist_pos--;
        navigate(history[hist_pos], 0);
    }
    return 1;
}

static int on_forward(struct widget *w, void *args, void *arg)
{
    if (hist_pos < hist_len - 1) {
        hist_pos++;
        navigate(history[hist_pos], 0);
    }
    return 1;
}

static int on_home(struct widget *w, void *args, void *arg) { navigate("/home", 1); return 1; }
static int on_root(struct widget *w, void *args, void *arg) { navigate("/", 1); return 1; }
static int on_go(struct widget *w, void *args, void *arg) { navigate(widget_text(path_field), 1); return 1; }
static int on_refresh(struct widget *w, void *args, void *arg) { refresh(); return 1; }

static int on_place(struct widget *w, void *args, void *arg)
{
    int i = ((struct sig_select *)args)->index;
    if (i >= 0 && i < (int)(sizeof places_list / sizeof places_list[0]))
        navigate(places_list[i].path, 1);
    return 1;
}

static int on_hidden(struct widget *w, void *args, void *arg)
{
    show_hidden = hidden_check->value;
    refresh();
    return 1;
}

static int on_toggle_hidden(struct widget *w, void *args, void *arg)
{
    widget_set_value(hidden_check, !hidden_check->value);
    return on_hidden(w, args, arg);
}

static int on_sort(struct widget *w, void *args, void *arg)
{
    m_sort(&model, (int)(long)arg, 0);
    refresh();
    return 1;
}

/* ---- handlers: file operations ---- */

static int on_new_folder(struct widget *w, void *args, void *arg)
{
    char name[NAME_MAX + 1];
    strlcpy(name, _("New folder"), sizeof name);
    if (!app_prompt(app, _("New folder"), _("Name:"), name, sizeof name) || !name[0])
        return 1;
    char path[512];
    fs_join(path, sizeof path, cwd, name);
    if (mkdir(path, 0755) < 0) {
        fail(_("Cannot create the folder"), errno);
        return 1;
    }
    log_line("mkdir %s", path, NULL);
    refresh();
    select_name(name);
    return 1;
}

static int on_new_file(struct widget *w, void *args, void *arg)
{
    char name[NAME_MAX + 1];
    strlcpy(name, _("New file.txt"), sizeof name);
    if (!app_prompt(app, _("New file"), _("Name:"), name, sizeof name) || !name[0])
        return 1;
    char path[512];
    fs_join(path, sizeof path, cwd, name);
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0644);
    if (fd < 0) {
        fail(_("Cannot create the file"), errno);
        return 1;
    }
    close(fd);
    log_line("create %s", path, NULL);
    refresh();
    select_name(name);
    return 1;
}

static int on_open(struct widget *w, void *args, void *arg) { return on_activate(w, args, arg); }

static int on_open_with(struct widget *w, void *args, void *arg)
{
    struct entry *e = selected_entry();
    if (!e)
        return 1;
    char program[128] = "/bin/";
    if (!app_prompt(app, _("Open with"), _("Program:"), program, sizeof program) || !program[0])
        return 1;
    char path[512];
    fs_join(path, sizeof path, cwd, e->name);
    log_line("open %s with %s", path, program);
    int r = mime_spawn((char *const[]){ program, path, NULL });
    if (r < 0)
        fail(_("Cannot start the program"), r);
    return 1;
}

static int on_terminal(struct widget *w, void *args, void *arg)
{
    struct entry *e = selected_entry();
    char path[512];
    if (e && e->dir)
        fs_join(path, sizeof path, cwd, e->name);
    else
        strlcpy(path, cwd, sizeof path);
    log_line("terminal in %s", path, NULL);
    int r = mime_spawn((char *const[]){ "/bin/term", "-d", path, NULL });
    if (r < 0)
        fail(_("Cannot start the terminal"), r);
    return 1;
}

static int on_rename(struct widget *w, void *args, void *arg)
{
    struct entry *e = selected_entry();
    if (!e)
        return 1;
    char name[NAME_MAX + 1];
    strlcpy(name, e->name, sizeof name);
    if (!app_prompt(app, _("Rename"), _("New name:"), name, sizeof name) || !name[0] || strcmp(name, e->name) == 0)
        return 1;
    char from[512], to[512];
    fs_join(from, sizeof from, cwd, e->name);
    fs_join(to, sizeof to, cwd, name);
    if (rename(from, to) < 0) {
        fail(_("Cannot rename"), errno);
        return 1;
    }
    log_line("rename %s -> %s", from, to);
    refresh();
    select_name(name);
    return 1;
}

static int on_delete(struct widget *w, void *args, void *arg)
{
    struct entry *e = selected_entry();
    if (!e)
        return 1;
    char text[300];
    if (e->dir)
        snprintf(text, sizeof text, _("Delete \"%s\" and everything in it?"), e->name);
    else
        snprintf(text, sizeof text, _("Delete \"%s\"?"), e->name);
    const char *const buttons[] = { _("Delete"), _("Cancel") };
    if (app_dialog(app, _("Delete"), text, buttons, 2) != 0)
        return 1;
    char path[512];
    fs_join(path, sizeof path, cwd, e->name);
    int r = fs_remove(path);
    if (r < 0) {
        fail(_("Cannot delete"), r);
        return 1;
    }
    log_line("delete %s", path, NULL);
    refresh();
    return 1;
}

static int on_copy(struct widget *w, void *args, void *arg)
{
    struct entry *e = selected_entry();
    if (!e)
        return 1;
    fs_join(clip_path, sizeof clip_path, cwd, e->name);
    clip_cut = (int)(long)arg;
    gui_clipboard_set(clip_path, (int)strlen(clip_path));
    return 1;
}

static int on_paste(struct widget *w, void *args, void *arg)
{
    if (!clip_path[0])
        return 1;
    char to[512];
    fs_join(to, sizeof to, cwd, fs_basename(clip_path));
    if (strcmp(to, clip_path) == 0) {
        if (clip_cut)
            return 1;
        char name[NAME_MAX + 1];
        snprintf(name, sizeof name, _("Copy of %s"), fs_basename(clip_path));
        fs_join(to, sizeof to, cwd, name);
    }
    struct stat st;
    if (stat(to, &st) == 0) {
        fail(_("Cannot paste"), EEXIST);
        return 1;
    }
    int r = clip_cut ? fs_move(clip_path, to) : fs_copy(clip_path, to);
    if (r < 0) {
        fail(clip_cut ? _("Cannot move") : _("Cannot copy"), r);
        return 1;
    }
    log_line(clip_cut ? "move %s -> %s" : "copy %s -> %s", clip_path, to);
    if (clip_cut) {
        strlcpy(clip_path, to, sizeof clip_path);
        clip_cut = 0;
    }
    refresh();
    select_name(fs_basename(to));
    return 1;
}

static struct widget *prop_win;
static int prop_done;
static int on_prop_close(struct widget *w, void *args, void *arg) { prop_done = 1; return 1; }

static void prop_row(struct widget *grid, int row, const char *name, const char *value)
{
    struct widget *l = label_new(grid, name);
    widget_set_grid(l, row, 0, 1, 1);
    widget_set_align(l, ALIGN_END, ALIGN_CENTER);
    struct widget *v = label_new(grid, value);
    widget_set_grid(v, row, 1, 1, 1);
}

static int on_properties(struct widget *w, void *args, void *arg)
{
    struct entry *e = selected_entry();
    char path[512], size[64], date[64], count[96], files_text[48], dirs_text[48];
    if (e)
        fs_join(path, sizeof path, cwd, e->name);
    else
        strlcpy(path, cwd, sizeof path);
    struct stat st;
    if (stat(path, &st) < 0) {
        fail(_("Cannot read the properties"), errno);
        return 1;
    }
    int files = 0, dirs = 0;
    long bytes = fs_tree_size(path, &files, &dirs);
    time_t t = (time_t)st.st_mtime;
    struct tm tm;
    localtime_r(&t, &tm);
    strftime(date, sizeof date, "%c", &tm);
    const char *type = mime_type(path, S_ISDIR(st.st_mode));
    prop_win = app_modal_window(app, win, 420, 7 * 26 + 60, _("Properties"));
    if (!prop_win)
        return 1;
    struct widget *grid = grid_new(prop_win);
    grid_set_stretch(grid, -1, 1, 1);
    prop_row(grid, 0, _("Name"), fs_basename(path));
    prop_row(grid, 1, _("Location"), cwd);
    prop_row(grid, 2, _("Type"), describe(type, (st.st_mode & 0111) != 0));
    if (S_ISDIR(st.st_mode)) {
        snprintf(files_text, sizeof files_text, ngettext("%d file", "%d files", files), files);
        snprintf(dirs_text, sizeof dirs_text, ngettext("%d folder", "%d folders", dirs - 1), dirs - 1);
        snprintf(count, sizeof count, "%s, %s", files_text, dirs_text);
        prop_row(grid, 3, _("Contents"), count);
        prop_row(grid, 4, _("Size"), fs_human_size(bytes, size, sizeof size));
    } else {
        snprintf(count, sizeof count, ngettext("%s (%ld byte)", "%s (%ld bytes)", bytes),
                 fs_human_size(bytes, size, sizeof size), bytes);
        prop_row(grid, 3, _("Size"), count);
        snprintf(count, sizeof count, "%lu", (unsigned long)st.st_ino);
        prop_row(grid, 4, _("Inode"), count);
    }
    prop_row(grid, 5, _("Modified"), date);
    struct widget *row = box_new(prop_win, 0);
    struct widget *ok = button_new(row, _("OK"));
    widget_set_align(ok, ALIGN_END, ALIGN_CENTER);
    widget_set_stretch(row, 1, 0);
    widget_connect(ok, "clicked", on_prop_close, NULL);
    widget_connect(prop_win, "close", on_prop_close, NULL);
    widget_focus(ok);
    prop_done = 0;
    while (!prop_done && app_step(app, -1))
        ;
    window_close(prop_win);
    app_step(app, 0);
    return 1;
}

static int on_close(struct widget *w, void *args, void *arg) { app_quit(app, 0); return 1; }

/* ---- keys, context menu, timer ---- */

static int on_context(struct widget *w, void *args, void *arg)
{
    struct sig_click *c = args;
    int x, y;
    widget_abs(w, &x, &y);
    menu_popup(selected_entry() ? item_menu : dir_menu, x + c->x, y + c->y);
    return 1;
}

static int on_key(struct widget *w, void *args, void *arg)
{
    struct sig_key *k = args;
    if (widget_focused(win) != table)
        return 0;
    if (k->ch == '\b' && !(k->mods & (WMOD_CTRL | WMOD_ALT)))
        return on_up(w, args, arg);
    if (k->ch >= ' ' && k->ch < 0x7f && !(k->mods & (WMOD_CTRL | WMOD_ALT))) {
        long now = uptime_ms();
        if (now - typed_ms > TYPEAHEAD_MS)
            typed[0] = '\0';
        typed_ms = now;
        size_t len = strlen(typed);
        if (len < sizeof typed - 1) {
            typed[len++] = (char)k->ch;
            typed[len] = '\0';
        }
        for (int i = 0; i < nentries; i++) {
            char prefix[64];
            strlcpy(prefix, entries[i].name, sizeof prefix);
            prefix[len] = '\0';
            if (name_cmp(prefix, typed) == 0) {
                view_select(table, i);
                break;
            }
        }
        return 1;
    }
    return 0;
}

static void on_tick(void *arg)
{
    struct entry *list = NULL;
    int hidden = 0;
    int n = scan(cwd, &list, &hidden);
    if (n >= 0 && !same_listing(list, n, entries, nentries))
        refresh();
    free(list);
}

/* ---- construction ---- */

static struct widget *item(struct widget *menu, const char *text, const char *icon, signal_fn fn, void *arg, int key,
                           int mods)
{
    struct widget *w = menu_add(menu, text, icon);
    widget_connect(w, "clicked", fn, arg);
    if (key)
        widget_set_accel(w, key, mods);
    return w;
}

static void build_menus(void)
{
    struct widget *bar = menubar_new(win);
    struct widget *file = menu_new(bar, _("File"));
    item(file, _("New folder..."), "folder", on_new_folder, NULL, KEY_N, WMOD_CTRL);
    item(file, _("New file..."), "new", on_new_file, NULL, 0, 0);
    menu_add_separator(file);
    item(file, _("Open"), "open", on_open, NULL, 0, 0);
    item(file, _("Open with..."), NULL, on_open_with, NULL, 0, 0);
    item(file, _("Open in terminal"), "terminal", on_terminal, NULL, KEY_T, WMOD_CTRL | WMOD_SHIFT);
    menu_add_separator(file);
    item(file, _("Rename..."), "edit", on_rename, NULL, KEY_F2, 0);
    item(file, _("Delete"), "quit", on_delete, NULL, KEY_DELETE, 0);
    item(file, _("Properties"), NULL, on_properties, NULL, KEY_ENTER, WMOD_ALT);
    menu_add_separator(file);
    item(file, _("Close"), NULL, on_close, NULL, KEY_W, WMOD_CTRL);
    struct widget *edit = menu_new(bar, _("Edit"));
    item(edit, _("Copy"), "copy", on_copy, (void *)0, KEY_C, WMOD_CTRL);
    item(edit, _("Cut"), "cut", on_copy, (void *)1, KEY_X, WMOD_CTRL);
    item(edit, _("Paste"), "paste", on_paste, NULL, KEY_V, WMOD_CTRL);
    struct widget *view = menu_new(bar, _("View"));
    item(view, _("Refresh"), "refresh", on_refresh, NULL, KEY_F5, 0);
    item(view, _("Show hidden files"), NULL, on_toggle_hidden, NULL, KEY_H, WMOD_CTRL);
    menu_add_separator(view);
    item(view, _("Sort by name"), NULL, on_sort, (void *)0, 0, 0);
    item(view, _("Sort by size"), NULL, on_sort, (void *)1, 0, 0);
    item(view, _("Sort by type"), NULL, on_sort, (void *)2, 0, 0);
    item(view, _("Sort by date"), NULL, on_sort, (void *)3, 0, 0);
    struct widget *go = menu_new(bar, _("Go"));
    item(go, _("Back"), "back", on_back, NULL, KEY_LEFT, WMOD_ALT);
    item(go, _("Forward"), "forward", on_forward, NULL, KEY_RIGHT, WMOD_ALT);
    item(go, _("Parent folder"), "up", on_up, NULL, KEY_UP, WMOD_ALT);
    menu_add_separator(go);
    item(go, _("Home"), "home", on_home, NULL, KEY_HOME, WMOD_ALT);
    item(go, _("Root"), NULL, on_root, NULL, 0, 0);

    item_menu = popupmenu_new(win);
    item(item_menu, _("Open"), "open", on_open, NULL, 0, 0);
    item(item_menu, _("Open with..."), NULL, on_open_with, NULL, 0, 0);
    item(item_menu, _("Open in terminal"), "terminal", on_terminal, NULL, 0, 0);
    menu_add_separator(item_menu);
    item(item_menu, _("Copy"), "copy", on_copy, (void *)0, 0, 0);
    item(item_menu, _("Cut"), "cut", on_copy, (void *)1, 0, 0);
    item(item_menu, _("Paste"), "paste", on_paste, NULL, 0, 0);
    menu_add_separator(item_menu);
    item(item_menu, _("Rename..."), "edit", on_rename, NULL, 0, 0);
    item(item_menu, _("Delete"), "quit", on_delete, NULL, 0, 0);
    menu_add_separator(item_menu);
    item(item_menu, _("Properties"), NULL, on_properties, NULL, 0, 0);

    dir_menu = popupmenu_new(win);
    item(dir_menu, _("New folder..."), "folder", on_new_folder, NULL, 0, 0);
    item(dir_menu, _("New file..."), "new", on_new_file, NULL, 0, 0);
    item(dir_menu, _("Open in terminal"), "terminal", on_terminal, NULL, 0, 0);
    menu_add_separator(dir_menu);
    item(dir_menu, _("Paste"), "paste", on_paste, NULL, 0, 0);
    menu_add_separator(dir_menu);
    item(dir_menu, _("Refresh"), "refresh", on_refresh, NULL, 0, 0);
    item(dir_menu, _("Properties"), NULL, on_properties, NULL, 0, 0);
}

static void build_toolbar(void)
{
    struct widget *bar = toolbar_new(win);
    back_button = toolbar_add(bar, "back", _("Back"));
    widget_connect(back_button, "clicked", on_back, NULL);
    forward_button = toolbar_add(bar, "forward", _("Forward"));
    widget_connect(forward_button, "clicked", on_forward, NULL);
    up_button = toolbar_add(bar, "up", _("Parent folder"));
    widget_connect(up_button, "clicked", on_up, NULL);
    widget_connect(toolbar_add(bar, "home", _("Home")), "clicked", on_home, NULL);
    path_field = textfield_new(bar, cwd);
    widget_set_stretch(path_field, 1, 0);
    widget_connect(path_field, "activate", on_go, NULL);
    widget_connect(toolbar_add(bar, "refresh", _("Refresh")), "clicked", on_refresh, NULL);
    hidden_check = checkbox_new(bar, _("Hidden"));
    widget_connect(hidden_check, "toggled", on_hidden, NULL);
}

int main(int argc, char **argv)
{
    if (argc > 1)
        strlcpy(cwd, argv[1], sizeof cwd);
    fs_normalize(cwd);
    app = app_create();
    if (!app)
        return 1;
    textdomain("files");
    char title[300];
    snprintf(title, sizeof title, _("%s - Files"), fs_basename(cwd));
    win = app_window(app, 680, 460, title);
    if (!win)
        return 1;
    build_menus();
    build_toolbar();
    struct widget *split = splitpane_new(win, 0);
    places = listview_new(split);
    for (size_t i = 0; i < sizeof places_list / sizeof places_list[0]; i++)
        listview_add(places, _(places_list[i].name));
    widget_connect(places, "selected", on_place, NULL);
    table = table_new(split);
    view_set_model(table, &model);
    table_set_column_width(table, 0, 220);
    table_set_column_width(table, 1, 80);
    table_set_column_width(table, 2, 95);
    table_set_column_width(table, 3, 125);
    widget_connect(table, "activate", on_activate, NULL);
    widget_connect(table, "selected", on_selected, NULL);
    widget_connect(table, "context", on_context, NULL);
    splitpane_set_position(split, 140);
    struct widget *sb = statusbar_new(win);
    status = statusbar_add(sb, 1);
    sel_label = statusbar_add(sb, 0);
    widget_connect(win, "key", on_key, NULL);
    strlcpy(history[0], cwd, sizeof history[0]);
    hist_len = 1;
    hist_pos = 0;
    refresh();
    widget_focus(table);
    app_timer_add(app, 2000, 1, on_tick, NULL);
    app_run(app);
    app_destroy(app);
    return 0;
}
