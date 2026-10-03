/* The folder view that the file manager and the file chooser share, made
 * of the places sidebar, the path bar, the location entry, the search,
 * the recent list and the table of entries (gui/folderview.h). */
#include <gui/app.h>
#include <gui/folderview.h>
#include <gui/mime.h>
#include <gui/model.h>
#include <dirent.h>
#include <errno.h>
#include <langinfo.h>
#include <libintl.h>
#include <limits.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "dialog.h"

/* tools/xgettext.py extracts these into the libgui domain. */
#define _(s) dgettext("libgui", s)
#define N_(s) s
#undef ngettext
#define ngettext(s, p, n) dngettext("libgui", s, p, n)

#ifndef PATH_MAX
#define PATH_MAX 512
#endif

#define MAX_PLACES 24
#define RECENT_MAX 50
#define RECENT_FILE "/.local/share/recent-files"
/* A search visits at most this many folders, this deep, for this many
 * results, so that a search from the root stays quick. */
#define SEARCH_DEPTH 6
#define SEARCH_DIRS 400
#define SEARCH_RESULTS 300
#define SIDEBAR_W 160
#define POLL_MS 2000

struct place {
    char label[64];
    char path[PATH_MAX];        /* empty for Recent */
    const char *icon;
    int group;                  /* places of another group are separated by a line */
};

struct seg {
    int x, w;
    int end;                    /* length of the crumb prefix it opens, 0: none */
    const char *icon;
    char label[NAME_MAX + 1];
    int current;
};

struct folderview {
    struct app *app;
    struct widget *win, *pathbar, *location, *search, *search_button, *sidebar, *table;
    char title[128];
    const struct folderview_ops *ops;
    void *arg;
    enum folderview_kind view;
    char cwd[PATH_MAX];         /* the folder shown, and the base of relative names */
    char crumb[PATH_MAX];       /* the deepest folder of the path bar, cwd or below it */
    char home[PATH_MAX];
    char search_root[PATH_MAX];
    struct folderview_entry *entries;
    int nentries, cap, nhidden;
    uint32_t signature;         /* of the folder as last listed, for the poll */
    int sort_col, sort_desc;    /* sort_col -1: the order of the listing (Recent) */
    int show_hidden;
    struct place place[MAX_PLACES];
    int nplaces, current_place;
    struct model model;
    int completing;             /* the location entry is being completed */
    int location_len;           /* its length before the last change */
    int ready;                  /* construction is over and callbacks run */
    struct timer *timer;
};

/* ---- paths ---- */

static void path_join(char *out, size_t size, const char *dir, const char *name)
{
    if (name[0] == '/')
        strlcpy(out, name, size);
    else if (strcmp(dir, "/") == 0)
        snprintf(out, size, "/%s", name);
    else
        snprintf(out, size, "%s/%s", dir, name);
}

/* Remove empty, "." and ".." components of an absolute path. */
static void path_normalize(char *path)
{
    char out[PATH_MAX];
    size_t n = 0;
    const char *p = path;
    out[0] = '\0';
    while (*p) {
        while (*p == '/')
            p++;
        const char *s = p;
        while (*p && *p != '/')
            p++;
        size_t len = (size_t)(p - s);
        if (len == 0 || (len == 1 && s[0] == '.'))
            continue;
        if (len == 2 && s[0] == '.' && s[1] == '.') {
            while (n > 0 && out[n - 1] != '/')
                n--;
            if (n > 0)
                n--;
            out[n] = '\0';
            continue;
        }
        if (n + 1 + len >= sizeof out)
            break;
        out[n++] = '/';
        memcpy(out + n, s, len);
        n += len;
        out[n] = '\0';
    }
    if (n == 0)
        strcpy(out, "/");
    strcpy(path, out);
}

void folderview_resolve(const struct folderview *fv, const char *text, char *out, size_t size)
{
    if (text[0] == '~' && (text[1] == '/' || text[1] == '\0'))
        snprintf(out, size, "%s%s", fv->home, text + 1);
    else
        path_join(out, size, fv->cwd, text);
    path_normalize(out);
}

static const char *path_base(const char *path)
{
    const char *s = strrchr(path, '/');
    return s && s[1] ? s + 1 : path;
}

static void path_dir(const char *path, char *out, size_t size)
{
    strlcpy(out, path, size);
    char *s = strrchr(out, '/');
    if (s == out)
        out[1] = '\0';
    else if (s)
        *s = '\0';
}

static int is_dir(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

/* The path with the home folder written as ~, for the Location column. */
static void path_display(const struct folderview *fv, const char *path, char *out, size_t size)
{
    size_t n = strlen(fv->home);
    if (n > 1 && strncmp(path, fv->home, n) == 0 && (path[n] == '/' || path[n] == '\0'))
        snprintf(out, size, "~%s", path + n);
    else
        strlcpy(out, path, size);
}

/* The home folder from HOME, or from the account database when HOME is
 * unset (docs/design/users.md). */
static void home_of(char *out, size_t size)
{
    const char *home = getenv("HOME");
    struct passwd *pw = home && home[0] == '/' ? NULL : getpwuid(getuid());
    strlcpy(out, home && home[0] == '/' ? home : pw && pw->pw_dir[0] ? pw->pw_dir : "/", size);
    path_normalize(out);
    if (!is_dir(out))
        strlcpy(out, "/", size);
}

/* ---- the recent list ---- */

static void recent_path(char *out, size_t size)
{
    char home[PATH_MAX];
    home_of(home, sizeof home);
    snprintf(out, size, "%s%s", strcmp(home, "/") == 0 ? "" : home, RECENT_FILE);
}

/* Read up to max paths, newest first, and return the count. */
static int recent_read(char (*list)[PATH_MAX], int max)
{
    char file[PATH_MAX];
    recent_path(file, sizeof file);
    FILE *f = fopen(file, "r");
    if (!f)
        return 0;
    int n = 0;
    while (n < max && fgets(list[n], PATH_MAX, f)) {
        list[n][strcspn(list[n], "\n")] = '\0';
        if (list[n][0] == '/')
            n++;
    }
    fclose(f);
    return n;
}

static void make_dirs(const char *dir)
{
    char p[PATH_MAX];
    strlcpy(p, dir, sizeof p);
    for (char *s = p + 1; *s; s++)
        if (*s == '/') {
            *s = '\0';
            mkdir(p, 0755);
            *s = '/';
        }
    mkdir(p, 0755);
}

void folderview_recent_add(const char *path)
{
    static char list[RECENT_MAX][PATH_MAX];
    int n = recent_read(list, RECENT_MAX);
    char file[PATH_MAX], dir[PATH_MAX];
    recent_path(file, sizeof file);
    path_dir(file, dir, sizeof dir);
    make_dirs(dir);
    FILE *f = fopen(file, "w");
    if (!f)
        return;
    fprintf(f, "%s\n", path);
    for (int i = 0, kept = 1; i < n && kept < RECENT_MAX; i++)
        if (strcmp(list[i], path) != 0) {
            fprintf(f, "%s\n", list[i]);
            kept++;
        }
    fclose(f);
}

/* ---- names of sizes and types ---- */

const char *folderview_format_size(long n, char *buf, size_t size)
{
    if (n < 1000) {
        snprintf(buf, size, ngettext("%ld byte", "%ld bytes", (unsigned long)n), n);
        return buf;
    }
    long unit = 1000;
    const char *format = _("%s kB");
    if (n >= 1000000000L) {
        unit = 1000000000L;
        format = _("%s GB");
    } else if (n >= 1000000L) {
        unit = 1000000L;
        format = _("%s MB");
    }
    /* The number uses the decimal separator of LC_NUMERIC. */
    long tenths = n / (unit / 10);
    char number[32];
    snprintf(number, sizeof number, "%ld%s%ld", tenths / 10, nl_langinfo(RADIXCHAR), tenths % 10);
    snprintf(buf, size, format, number);
    return buf;
}

const char *folderview_describe(const char *type, int exec)
{
    static const struct { const char *type, *desc; } names[] = {
        { MIME_DIRECTORY, N_("Folder") }, { "text/plain", N_("Text") }, { "text/x-csrc", N_("C source") },
        { "text/x-shellscript", N_("Shell script") }, { "text/x-lua", N_("Lua source") },
        { "image/png", N_("PNG image") }, { "image/bmp", N_("BMP image") }, { "image/svg+xml", N_("SVG image") },
        { "audio/x-wav", N_("WAV audio") }, { "audio/flac", N_("FLAC audio") }, { "audio/ogg", N_("Ogg audio") },
        { MIME_LAUNCHER, N_("Launcher") }, { "application/x-minios-package", N_("Package") },
    };
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++)
        if (strcmp(names[i].type, type) == 0)
            return _(names[i].desc);
    if (strcmp(type, "application/octet-stream") == 0)
        return exec ? _("Program") : _("File");
    return type;
}

/* ---- the listing ---- */

static int is_hidden(const char *name)
{
    size_t n = strlen(name);
    return name[0] == '.' || (n > 1 && name[n - 1] == '~');
}

static void entries_clear(struct folderview *fv)
{
    for (int i = 0; i < fv->nentries; i++)
        free(fv->entries[i].path);
    fv->nentries = 0;
}

/* Add path when the program's filter admits it, which folders always pass. */
static void entry_add(struct folderview *fv, const char *path)
{
    struct stat st;
    if (stat(path, &st) < 0)
        return;
    int dir = S_ISDIR(st.st_mode);
    if (!dir && fv->ops && fv->ops->filter && !fv->ops->filter(fv, path_base(path), fv->arg))
        return;
    if (fv->nentries == fv->cap) {
        int cap = fv->cap ? fv->cap * 2 : 64;
        struct folderview_entry *n = realloc(fv->entries, (size_t)cap * sizeof *n);
        if (!n)
            return;
        fv->entries = n;
        fv->cap = cap;
    }
    struct folderview_entry *e = &fv->entries[fv->nentries];
    e->path = strdup(path);
    if (!e->path)
        return;
    e->name = path_base(e->path);
    e->size = (long)st.st_size;
    e->mtime = st.st_mtime;
    e->dir = dir;
    e->exec = (st.st_mode & 0111) != 0;
    const char *type = mime_type(e->name, dir);
    e->desc = folderview_describe(type, e->exec);
    e->icon = icon_get(mime_icon(type));
    fv->nentries++;
}

static uint32_t mix(uint32_t h, const void *data, size_t n)
{
    const unsigned char *p = data;
    for (size_t i = 0; i < n; i++)
        h = (h ^ p[i]) * 16777619u;
    return h;
}

/* List a folder, or with list 0 only compute the signature of what it
 * would list, a hash of the names, sizes, times and kinds in directory
 * order. */
static int scan_folder(struct folderview *fv, const char *dir, int list, uint32_t *signature, int *hidden)
{
    DIR *d = opendir(dir);
    if (!d)
        return -errno;
    uint32_t h = 2166136261u;
    int nh = 0;
    struct dirent *de;
    char path[PATH_MAX];
    while ((de = readdir(d))) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
            continue;
        if (is_hidden(de->d_name)) {
            nh++;
            if (!fv->show_hidden)
                continue;
        }
        path_join(path, sizeof path, dir, de->d_name);
        struct stat st;
        h = mix(h, de->d_name, strlen(de->d_name) + 1);
        if (stat(path, &st) == 0) {
            int64_t v[3] = { st.st_size, st.st_mtime, S_ISDIR(st.st_mode) };
            h = mix(h, v, sizeof v);
        }
        if (list)
            entry_add(fv, path);
    }
    closedir(d);
    *signature = h;
    *hidden = nh;
    return 0;
}

static void list_recent(struct folderview *fv)
{
    static char list[RECENT_MAX][PATH_MAX];
    int n = recent_read(list, RECENT_MAX);
    for (int i = 0; i < n; i++)
        entry_add(fv, list[i]);
}

/* Case insensitive (ASCII) substring test. */
static int name_contains(const char *name, const char *query)
{
    size_t n = strlen(query);
    for (; *name; name++) {
        size_t i = 0;
        while (i < n) {
            int a = (unsigned char)name[i], b = (unsigned char)query[i];
            if (a >= 'A' && a <= 'Z') a += 32;
            if (b >= 'A' && b <= 'Z') b += 32;
            if (a != b)
                break;
            i++;
        }
        if (i == n)
            return 1;
    }
    return 0;
}

static void search_walk(struct folderview *fv, const char *dir, const char *query, int depth, int *dirs)
{
    if (depth > SEARCH_DEPTH || ++*dirs > SEARCH_DIRS)
        return;
    DIR *d = opendir(dir);
    if (!d)
        return;
    struct dirent *de;
    char path[PATH_MAX];
    while ((de = readdir(d)) && fv->nentries < SEARCH_RESULTS) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
            continue;
        if (is_hidden(de->d_name) && !fv->show_hidden)
            continue;
        path_join(path, sizeof path, dir, de->d_name);
        if (name_contains(de->d_name, query))
            entry_add(fv, path);
        if (de->d_type == DT_DIR || (de->d_type == DT_UNKNOWN && is_dir(path)))
            search_walk(fv, path, query, depth + 1, dirs);
    }
    closedir(d);
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

static struct folderview *sorting;  /* the folder view of the running qsort */

static int entry_cmp(const void *pa, const void *pb)
{
    const struct folderview_entry *a = pa, *b = pb;
    struct folderview *fv = sorting;
    if (a->dir != b->dir && fv->view == FOLDERVIEW_FOLDER)
        return b->dir - a->dir;
    int r = 0;
    if (fv->sort_col == 1)
        r = fv->view == FOLDERVIEW_FOLDER ? (a->size < b->size ? -1 : a->size > b->size) : strcmp(a->path, b->path);
    else if (fv->sort_col == 2)
        r = strcmp(a->desc, b->desc);
    else if (fv->sort_col == 3)
        r = a->mtime < b->mtime ? -1 : a->mtime > b->mtime;
    if (r == 0)
        r = name_cmp(a->name, b->name);
    return fv->sort_desc ? -r : r;
}

static void entries_sort(struct folderview *fv)
{
    if (fv->sort_col < 0 || fv->nentries < 2)
        return;
    sorting = fv;
    qsort(fv->entries, (size_t)fv->nentries, sizeof *fv->entries, entry_cmp);
    sorting = NULL;
}

/* ---- the table model with Name, Size or Location, Type and Modified ---- */

/* The time of today, Yesterday, the day of this year or the full date. */
static const char *short_time(int64_t when, char *buf, size_t size)
{
    time_t t = (time_t)when, now = time(NULL);
    struct tm tm, today;
    localtime_r(&t, &tm);
    localtime_r(&now, &today);
    const char *format;
    if (tm.tm_year == today.tm_year && tm.tm_yday == today.tm_yday - 1) {
        strlcpy(buf, _("Yesterday"), size);
        return buf;
    }
    if (tm.tm_year == today.tm_year && tm.tm_yday == today.tm_yday)
        format = "%H:%M";
    else if (tm.tm_year == today.tm_year)
        format = _("%e %b");
    else
        format = _("%e %b %Y");
    strftime(buf, size, format, &tm);
    char *s = buf;
    while (*s == ' ')
        s++;
    return s;
}

static int m_rows(struct model *m, int parent)
{
    struct folderview *fv = m->user;
    return parent < 0 ? fv->nentries : 0;
}
static int m_child(struct model *m, int parent, int index) { return index; }
static int m_columns(struct model *m) { return 4; }

static const char *m_cell(struct model *m, int row, int col, char *buf, size_t size)
{
    struct folderview *fv = m->user;
    if (row < 0 || row >= fv->nentries)
        return "";
    struct folderview_entry *e = &fv->entries[row];
    switch (col) {
    case 0:
        return e->name;
    case 1:
        if (fv->view == FOLDERVIEW_FOLDER)
            return e->dir ? "" : folderview_format_size(e->size, buf, size);
        char dir[PATH_MAX];
        path_dir(e->path, dir, sizeof dir);
        path_display(fv, dir, buf, size);
        return buf;
    case 2:
        return e->desc;
    default:
        return short_time(e->mtime, buf, size);
    }
}

static const char *m_header(struct model *m, int col)
{
    struct folderview *fv = m->user;
    static const char *const folder[] = { N_("Name"), N_("Size"), N_("Type"), N_("Modified") };
    static const char *const other[] = { N_("Name"), N_("Location"), N_("Type"), N_("Modified") };
    return _((fv->view == FOLDERVIEW_FOLDER ? folder : other)[col]);
}

static void m_sort(struct model *m, int col, int desc)
{
    struct folderview *fv = m->user;
    fv->sort_col = col;
    fv->sort_desc = desc;
    entries_sort(fv);
}

static const struct image *m_icon(struct model *m, int row)
{
    struct folderview *fv = m->user;
    return row >= 0 && row < fv->nentries ? fv->entries[row].icon : NULL;
}

/* ---- the view state ---- */

static void message(struct folderview *fv, const char *text)
{
    const char *const buttons[] = { _("OK") };
    dialog_message(fv->app, fv->win, fv->title, text, buttons, 1);
}

static void notify_changed(struct folderview *fv)
{
    if (fv->ready && fv->ops && fv->ops->changed)
        fv->ops->changed(fv, fv->arg);
}

static void notify_selected(struct folderview *fv)
{
    if (fv->ready && fv->ops && fv->ops->selected)
        fv->ops->selected(fv, fv->arg);
}

static void update_place(struct folderview *fv)
{
    int found = -1;
    for (int i = 0; i < fv->nplaces && found < 0; i++)
        if (fv->view == FOLDERVIEW_RECENT ? fv->place[i].path[0] == '\0'
                                          : fv->view == FOLDERVIEW_FOLDER && strcmp(fv->place[i].path, fv->cwd) == 0)
            found = i;
    fv->current_place = found;
    widget_invalidate(fv->sidebar);
}

void folderview_select_name(struct folderview *fv, const char *name)
{
    for (int i = 0; i < fv->nentries; i++)
        if (strcmp(fv->entries[i].name, name) == 0) {
            view_select(fv->table, i);
            return;
        }
}

/* Reread the current view and report the errors of a folder. */
static void reload(struct folderview *fv)
{
    entries_clear(fv);
    int err = 0;
    fv->nhidden = 0;
    if (fv->view == FOLDERVIEW_RECENT) {
        list_recent(fv);
    } else if (fv->view == FOLDERVIEW_SEARCH && widget_text(fv->search)[0]) {
        int dirs = 0;
        search_walk(fv, fv->search_root, widget_text(fv->search), 0, &dirs);
    } else {
        int hidden = 0;
        err = scan_folder(fv, fv->view == FOLDERVIEW_SEARCH ? fv->search_root : fv->cwd, 1, &fv->signature, &hidden);
        if (!fv->show_hidden)
            fv->nhidden = hidden;
    }
    entries_sort(fv);
    fv->table->value = -1;
    view_refresh(fv->table);
    if (fv->nentries)
        view_scroll_to(fv->table, 0);
    update_place(fv);
    widget_invalidate(fv->pathbar);
    notify_changed(fv);
    notify_selected(fv);
    if (err < 0) {
        char text[PATH_MAX + 80];
        snprintf(text, sizeof text, _("The folder “%s” cannot be read: %s."), fv->cwd, strerror(-err));
        message(fv, text);
    }
}

void folderview_reload(struct folderview *fv)
{
    char keep[NAME_MAX + 1] = "";
    const struct folderview_entry *e = folderview_selected(fv);
    if (e)
        strlcpy(keep, e->name, sizeof keep);
    int scroll = view_scroll_position(fv->table);
    reload(fv);
    if (fv->nentries)
        view_scroll_to(fv->table, scroll < fv->nentries ? scroll : fv->nentries - 1);
    if (keep[0])
        folderview_select_name(fv, keep);
}

/* Called every two seconds, it rereads the folder when what it holds changed. */
static void poll(void *arg)
{
    struct folderview *fv = arg;
    if (fv->view != FOLDERVIEW_FOLDER)
        return;
    uint32_t signature;
    int hidden;
    if (scan_folder(fv, fv->cwd, 0, &signature, &hidden) == 0 && signature != fv->signature)
        folderview_reload(fv);
}

static void show_bar(struct folderview *fv, struct widget *which)
{
    widget_set_visible(fv->pathbar, which == fv->pathbar);
    widget_set_visible(fv->location, which == fv->location);
    widget_set_visible(fv->search, which == fv->search);
}

/* Show a folder.  The path bar keeps the folders below it when the folder
 * is one of those it already shows. */
int folderview_navigate(struct folderview *fv, const char *path)
{
    char target[PATH_MAX];
    folderview_resolve(fv, path, target, sizeof target);
    struct stat st;
    int err = stat(target, &st) < 0 ? errno : S_ISDIR(st.st_mode) ? 0 : ENOTDIR;
    if (err) {
        char text[PATH_MAX + 80];
        snprintf(text, sizeof text, _("The folder “%s” cannot be opened: %s."), target, strerror(err));
        message(fv, text);
        return -err;
    }
    char previous[PATH_MAX];
    strlcpy(previous, fv->cwd, sizeof previous);
    size_t n = strlen(target);
    int below = strncmp(fv->crumb, target, n) == 0 &&
                (fv->crumb[n] == '/' || fv->crumb[n] == '\0' || strcmp(target, "/") == 0);
    if (!below)
        strlcpy(fv->crumb, target, sizeof fv->crumb);
    strlcpy(fv->cwd, target, sizeof fv->cwd);
    if (fv->view != FOLDERVIEW_FOLDER) {
        if (fv->view == FOLDERVIEW_SEARCH)
            widget_set_text(fv->search, "");
        fv->view = FOLDERVIEW_FOLDER;
        if (fv->sort_col < 0)
            fv->sort_col = 0;
    }
    show_bar(fv, fv->pathbar);
    reload(fv);
    /* Going up selects the folder just left. */
    size_t m = strlen(fv->cwd);
    if (strncmp(previous, fv->cwd, m) == 0 && strlen(previous) > m && (m == 1 || previous[m] == '/')) {
        char first[NAME_MAX + 1];
        const char *rest = previous + m;
        while (*rest == '/')
            rest++;
        strlcpy(first, rest, sizeof first);
        first[strcspn(first, "/")] = '\0';
        folderview_select_name(fv, first);
    }
    return 0;
}

void folderview_show_recent(struct folderview *fv)
{
    if (fv->view == FOLDERVIEW_SEARCH)
        widget_set_text(fv->search, "");
    fv->view = FOLDERVIEW_RECENT;
    fv->sort_col = -1;
    show_bar(fv, fv->pathbar);
    reload(fv);
}

void folderview_up(struct folderview *fv)
{
    if (fv->view != FOLDERVIEW_FOLDER || strcmp(fv->cwd, "/") == 0)
        return;
    char up[PATH_MAX];
    path_dir(fv->cwd, up, sizeof up);
    folderview_navigate(fv, up);
}

void folderview_home(struct folderview *fv)
{
    folderview_navigate(fv, fv->home);
}

static void activate(struct folderview *fv, const struct folderview_entry *e)
{
    char path[PATH_MAX];
    strlcpy(path, e->path, sizeof path);
    if (e->dir) {
        folderview_navigate(fv, path);
        widget_focus(fv->table);
    } else if (fv->ops && fv->ops->open) {
        fv->ops->open(fv, path, fv->arg);
    }
}

void folderview_visit(struct folderview *fv)
{
    const struct folderview_entry *e = folderview_selected(fv);
    if (!e)
        return;
    char dir[PATH_MAX], name[NAME_MAX + 1];
    path_dir(e->path, dir, sizeof dir);
    strlcpy(name, e->name, sizeof name);
    folderview_navigate(fv, dir);
    folderview_select_name(fv, name);
    widget_focus(fv->table);
}

/* ---- the search ---- */

void folderview_search(struct folderview *fv, const char *text)
{
    if (fv->view != FOLDERVIEW_SEARCH) {
        strlcpy(fv->search_root, fv->view == FOLDERVIEW_RECENT ? fv->home : fv->cwd, sizeof fv->search_root);
        fv->view = FOLDERVIEW_SEARCH;
        fv->sort_col = 0;
        fv->sort_desc = 0;
    }
    show_bar(fv, fv->search);
    widget_set_text(fv->search, text);
    textfield_select(fv->search, -1, -1);
    widget_focus(fv->search);
    reload(fv);
}

static void stop_search(struct folderview *fv)
{
    char dir[PATH_MAX];
    strlcpy(dir, fv->search_root, sizeof dir);
    folderview_navigate(fv, dir);
    widget_focus(fv->table);
}

static int on_search_changed(struct widget *w, void *args, void *arg)
{
    reload(arg);
    return 0;
}

/* Enter opens the selected result, the first when none is selected. */
static int on_search_activate(struct widget *w, void *args, void *arg)
{
    struct folderview *fv = arg;
    if (!fv->nentries)
        return 1;
    if (!folderview_selected(fv))
        view_select(fv->table, 0);
    widget_focus(fv->table);
    activate(fv, folderview_selected(fv));
    return 1;
}

static int on_search_button(struct widget *w, void *args, void *arg)
{
    struct folderview *fv = arg;
    if (fv->view == FOLDERVIEW_SEARCH)
        stop_search(fv);
    else
        folderview_search(fv, "");
    return 1;
}

/* ---- the location entry ---- */

/* Inline completion appends and selects the longest common continuation
 * of the names in the folder of the text whenever the text grew at its
 * end, and typing on replaces the selected part. */
static void complete_location(struct folderview *fv)
{
    const char *text = widget_text(fv->location);
    int len = (int)strlen(text), grew = len > fv->location_len;
    fv->location_len = len;
    if (!grew || fv->completing)
        return;
    const char *slash = strrchr(text, '/');
    if (!slash || !slash[1])
        return;
    char dir[PATH_MAX], prefix[NAME_MAX + 1], typed[PATH_MAX];
    strlcpy(typed, text, sizeof typed);
    typed[slash - text + 1] = '\0';
    folderview_resolve(fv, typed, dir, sizeof dir);
    strlcpy(prefix, slash + 1, sizeof prefix);
    size_t plen = strlen(prefix);
    DIR *d = opendir(dir);
    if (!d)
        return;
    char common[NAME_MAX + 1] = "";
    int matches = 0, common_dir = 0;
    struct dirent *de;
    while ((de = readdir(d))) {
        if (strncmp(de->d_name, prefix, plen) != 0 || strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
            continue;
        if (is_hidden(de->d_name) && prefix[0] != '.')
            continue;
        if (matches++ == 0) {
            strlcpy(common, de->d_name, sizeof common);
            char full[PATH_MAX];
            path_join(full, sizeof full, dir, de->d_name);
            common_dir = is_dir(full);
        } else {
            size_t i = 0;
            while (common[i] && common[i] == de->d_name[i])
                i++;
            common[i] = '\0';
        }
    }
    closedir(d);
    if (!matches || strlen(common) <= plen)
        return;
    char out[PATH_MAX];
    snprintf(out, sizeof out, "%s%s%s", text, common + plen, matches == 1 && common_dir ? "/" : "");
    fv->completing = 1;
    widget_set_text(fv->location, out);
    textfield_select(fv->location, len, (int)strlen(out));
    fv->completing = 0;
}

static int on_location_changed(struct widget *w, void *args, void *arg)
{
    struct folderview *fv = arg;
    complete_location(fv);
    notify_selected(fv);
    return 0;
}

/* A folder is shown, a file opened. */
static int on_location_activate(struct widget *w, void *args, void *arg)
{
    struct folderview *fv = arg;
    const char *text = widget_text(fv->location);
    if (!text[0])
        return 1;
    char path[PATH_MAX];
    folderview_resolve(fv, text, path, sizeof path);
    struct stat st;
    if (stat(path, &st) < 0) {
        char msg[PATH_MAX + 80];
        snprintf(msg, sizeof msg, _("The file “%s” does not exist."), path);
        message(fv, msg);
        return 1;
    }
    if (S_ISDIR(st.st_mode)) {
        folderview_navigate(fv, path);
        widget_focus(fv->table);
    } else if (fv->ops && fv->ops->open) {
        fv->ops->open(fv, path, fv->arg);
    }
    return 1;
}

void folderview_location(struct folderview *fv, const char *text)
{
    if (fv->view == FOLDERVIEW_SEARCH)
        stop_search(fv);
    show_bar(fv, fv->location);
    if (text) {
        widget_set_text(fv->location, text);
    } else {
        char buf[PATH_MAX];
        snprintf(buf, sizeof buf, "%s%s", fv->cwd, strcmp(fv->cwd, "/") == 0 ? "" : "/");
        widget_set_text(fv->location, buf);
    }
    fv->location_len = (int)strlen(widget_text(fv->location));
    textfield_select(fv->location, -1, -1);
    widget_focus(fv->location);
}

/* ---- table and window handlers ---- */

static int on_activate(struct widget *w, void *args, void *arg)
{
    struct folderview *fv = arg;
    const struct folderview_entry *e = folderview_selected(fv);
    if (e)
        activate(fv, e);
    return 1;
}

static int on_selected(struct widget *w, void *args, void *arg)
{
    notify_selected(arg);
    return 0;
}

static void go_place(struct folderview *fv, int i)
{
    if (i < 0 || i >= fv->nplaces)
        return;
    if (fv->place[i].path[0])
        folderview_navigate(fv, fv->place[i].path);
    else
        folderview_show_recent(fv);
}

/* Keys that no widget consumed, offered to the program first. */
static int on_key(struct widget *w, void *args, void *arg)
{
    struct folderview *fv = arg;
    struct sig_key *k = args;
    if (fv->ops && fv->ops->key && fv->ops->key(fv, k, fv->arg))
        return 1;
    int ctrl = k->mods & WMOD_CTRL, alt = k->mods & WMOD_ALT;
    struct widget *focus = widget_focused(fv->win);
    int over_list = focus == fv->table || focus == fv->sidebar;
    if (k->code == KEY_ESC) {
        if (fv->view == FOLDERVIEW_SEARCH) {
            stop_search(fv);
            return 1;
        }
        if (fv->location->visible) {
            show_bar(fv, fv->pathbar);
            widget_focus(fv->table);
            notify_selected(fv);
            return 1;
        }
        return 0;
    }
    if (ctrl && k->code == KEY_L) {
        folderview_location(fv, NULL);
        return 1;
    }
    if (ctrl && k->code == KEY_H) {
        folderview_set_hidden(fv, !fv->show_hidden);
        return 1;
    }
    if (ctrl && k->code == KEY_F) {
        folderview_search(fv, widget_text(fv->search));
        return 1;
    }
    if ((alt && k->code == KEY_UP) || (over_list && k->ch == '\b' && !ctrl && !alt)) {
        folderview_up(fv);
        return 1;
    }
    if (alt && k->code == KEY_DOWN && fv->view == FOLDERVIEW_FOLDER && strlen(fv->crumb) > strlen(fv->cwd)) {
        char down[PATH_MAX];
        size_t n = strlen(fv->cwd) + (strcmp(fv->cwd, "/") != 0);
        strlcpy(down, fv->crumb, sizeof down);
        down[n + strcspn(down + n, "/")] = '\0';
        folderview_navigate(fv, down);
        return 1;
    }
    if (alt && k->code == KEY_HOME) {
        folderview_home(fv);
        return 1;
    }
    /* Typing over the table or the sidebar starts a search, or with / or
     * ~ the location entry. */
    if (k->ch >= 32 && k->ch < 127 && !ctrl && !alt && over_list) {
        char s[2] = { (char)k->ch, '\0' };
        if (k->ch == '/' || k->ch == '~')
            folderview_location(fv, s);
        else
            folderview_search(fv, s);
        return 1;
    }
    return 0;
}

/* ---- the places sidebar ---- */

struct sidebar {
    struct widget w;
    struct folderview *fv;
    int hover;
};

#define GROUP_GAP 9

static int row_h(const struct widget *w) { return widget_theme(w)->font->height + 10; }

static int place_y(struct sidebar *sb, int i)
{
    struct folderview *fv = sb->fv;
    int y = 4;
    for (int k = 0; k < i; k++)
        y += row_h(&sb->w) + (fv->place[k + 1].group != fv->place[k].group ? GROUP_GAP : 0);
    return y;
}

static int place_at(struct sidebar *sb, int y)
{
    for (int i = 0; i < sb->fv->nplaces; i++) {
        int top = place_y(sb, i);
        if (y >= top && y < top + row_h(&sb->w))
            return i;
    }
    return -1;
}

static void sidebar_measure(struct widget *w, struct size_hint *h)
{
    struct sidebar *sb = (struct sidebar *)w;
    h->pref_w = SIDEBAR_W;
    h->min_w = 80;
    h->pref_h = sb->fv->nplaces ? place_y(sb, sb->fv->nplaces - 1) + row_h(w) + 4 : 20;
    h->min_h = row_h(w);
}

static void sidebar_paint(struct widget *w, struct painter *p)
{
    struct sidebar *sb = (struct sidebar *)w;
    struct folderview *fv = sb->fv;
    const struct theme *t = p->theme;
    painter_fill(p, 0, 0, w->w, w->h, t->color[TC_WINDOW]);
    int rh = row_h(w);
    for (int i = 0; i < fv->nplaces; i++) {
        int y = place_y(sb, i);
        if (i > 0 && fv->place[i].group != fv->place[i - 1].group)
            painter_line(p, 8, y - GROUP_GAP / 2 - 1, w->w - 8, y - GROUP_GAP / 2 - 1, t->color[TC_BORDER]);
        int current = i == fv->current_place;
        if (current)
            painter_rounded(p, 4, y, w->w - 8, rh, t->color[TC_SELECTION], 0xffffffffu);
        else if (i == sb->hover)
            painter_rounded(p, 4, y, w->w - 8, rh, t->color[TC_BUTTON_HOVER], 0xffffffffu);
        const struct image *icon = icon_get(fv->place[i].icon);
        int x = 12;
        painter_push(p, 0, y, w->w - 8, rh);
        if (icon) {
            painter_image(p, x, (rh - image_lh(icon)) / 2, icon);
            x += image_lw(icon) + 8;
        }
        painter_text(p, x, (rh - painter_text_height(p)) / 2, fv->place[i].label,
                     t->color[current ? TC_SELECTION_TEXT : TC_TEXT]);
        painter_pop(p);
    }
    if (w->focused)
        painter_focus_ring(p, 1, 1, w->w - 2, w->h - 2);
}

static int sidebar_event(struct widget *w, struct event *e)
{
    struct sidebar *sb = (struct sidebar *)w;
    struct folderview *fv = sb->fv;
    switch (e->type) {
    case EV_MOUSE_DOWN:
        if (!(e->button & 1))
            return 0;
        go_place(fv, place_at(sb, e->y));
        return 1;
    case EV_MOUSE_MOVE: {
        int h = place_at(sb, e->y);
        if (h != sb->hover) {
            sb->hover = h;
            widget_invalidate(w);
        }
        return 1;
    }
    case EV_LEAVE:
        sb->hover = -1;
        widget_invalidate(w);
        return 1;
    case EV_KEY_DOWN:
        if (e->mods & WMOD_ALT)
            return 0;
        if (e->code == KEY_UP && fv->current_place > 0) {
            go_place(fv, fv->current_place - 1);
            widget_focus(w);
            return 1;
        }
        if (e->code == KEY_DOWN && fv->current_place < fv->nplaces - 1) {
            go_place(fv, fv->current_place + 1);
            widget_focus(w);
            return 1;
        }
        return 0;
    case EV_FOCUS_IN: case EV_FOCUS_OUT:
        widget_invalidate(w);
        return 1;
    default:
        return 0;
    }
}

/* The folder view lives as long as its sidebar. */
static void sidebar_destroy(struct widget *w)
{
    struct folderview *fv = ((struct sidebar *)w)->fv;
    app_timer_remove(fv->app, fv->timer);
    entries_clear(fv);
    free(fv->entries);
    free(fv);
}

static const struct widget_class sidebar_class = {
    "sidebar", sizeof(struct sidebar), sidebar_measure, NULL, sidebar_paint, sidebar_event, sidebar_destroy,
};

static void add_place(struct folderview *fv, const char *label, const char *path, const char *icon, int group)
{
    if (fv->nplaces == MAX_PLACES)
        return;
    struct place *p = &fv->place[fv->nplaces++];
    strlcpy(p->label, label, sizeof p->label);
    strlcpy(p->path, path, sizeof p->path);
    p->icon = icon;
    p->group = group;
}

/* Recent, the home folder and the usual folders below it that exist,
 * then the system folders, the mounted volumes other than the root, the
 * home volume and devfs, and last the root. */
static void build_places(struct folderview *fv)
{
    static const struct { const char *dir, *label, *icon; } user_dirs[] = {
        { "desktop", N_("Desktop"), "wallpaper" }, { "Desktop", N_("Desktop"), "wallpaper" },
        { "Documents", N_("Documents"), "documents" }, { "Downloads", N_("Downloads"), "downloads" },
        { "Music", N_("Music"), "music" }, { "Pictures", N_("Pictures"), "pictures" },
        { "Videos", N_("Videos"), "videos" },
    };
    static const struct { const char *path, *label, *icon; } system_dirs[] = {
        { "/bin", N_("Programs"), "terminal" }, { "/usr/share", N_("Shared files"), "documents" },
        { "/etc/fonts", N_("Fonts"), "app-unicode" }, { "/dev", N_("Devices"), "drive" },
    };
    add_place(fv, _("Recent"), "", "recent", 0);
    add_place(fv, _("Home"), fv->home, "home", 0);
    char path[PATH_MAX];
    for (size_t i = 0; i < sizeof user_dirs / sizeof user_dirs[0]; i++) {
        path_join(path, sizeof path, fv->home, user_dirs[i].dir);
        if (is_dir(path))
            add_place(fv, _(user_dirs[i].label), path, user_dirs[i].icon, 0);
    }
    for (size_t i = 0; i < sizeof system_dirs / sizeof system_dirs[0]; i++)
        if (is_dir(system_dirs[i].path))
            add_place(fv, _(system_dirs[i].label), system_dirs[i].path, system_dirs[i].icon, 1);
    FILE *f = fopen("/dev/mounts", "r");
    if (f) {
        char mp[256], type[32];
        unsigned long total, avail, bs;
        while (fscanf(f, "%255s %31s %lu %lu %lu", mp, type, &total, &avail, &bs) == 5)
            if (strcmp(mp, "/") != 0 && strcmp(mp, fv->home) != 0 && strcmp(type, "devfs") != 0 &&
                strncmp(mp, "/dev", 4) != 0)
                add_place(fv, path_base(mp), mp, "drive", 2);
        fclose(f);
    }
    add_place(fv, _("Computer"), "/", "drive", 3);
}

/* ---- the path bar ---- */

struct pathbar {
    struct widget w;
    struct folderview *fv;
    int hover;
    struct seg seg[64];
    int nseg;
};

#define SEG_PAD 8
#define SEP_W 14

static void seg_measure(struct widget *w, struct seg *s)
{
    const struct theme *t = widget_theme(w);
    const struct image *icon = s->icon ? icon_get(s->icon) : NULL;
    s->w = 2 * SEG_PAD + (s->label[0] ? gfx_text_width_font(t->font, s->label, -1) : 0) +
           (icon ? image_lw(icon) + (s->label[0] ? 6 : 0) : 0);
}

/* Split the crumb into buttons, Home or the root followed by one button
 * per folder.  Leading buttons that do not fit give way to one that opens
 * the folder above the first shown. */
static void pathbar_build(struct pathbar *pb)
{
    struct folderview *fv = pb->fv;
    struct seg all[64];
    int n = 0;
    if (fv->view == FOLDERVIEW_RECENT) {
        all[n] = (struct seg){ .end = 0, .icon = "recent", .current = 1 };
        strlcpy(all[n].label, _("Recent"), sizeof all[n].label);
        n++;
    } else {
        const char *p = fv->crumb;
        size_t hl = strlen(fv->home);
        if (hl > 1 && strncmp(p, fv->home, hl) == 0 && (p[hl] == '/' || p[hl] == '\0')) {
            all[n] = (struct seg){ .end = (int)hl, .icon = "home" };
            strlcpy(all[n].label, _("Home"), sizeof all[n].label);
            p += hl;
        } else {
            all[n] = (struct seg){ .end = 1, .icon = "drive" };
            all[n].label[0] = '\0';
        }
        n++;
        while (*p && n < 64) {
            while (*p == '/')
                p++;
            if (!*p)
                break;
            size_t len = strcspn(p, "/");
            all[n] = (struct seg){ .end = (int)(p - fv->crumb + len) };
            size_t copy = len < NAME_MAX ? len : NAME_MAX;
            memcpy(all[n].label, p, copy);
            all[n].label[copy] = '\0';
            n++;
            p += len;
        }
        size_t cl = strlen(fv->cwd);
        for (int i = 0; i < n; i++)
            all[i].current = fv->view == FOLDERVIEW_FOLDER && (size_t)all[i].end == cl;
    }
    int total = 0;
    for (int i = 0; i < n; i++) {
        seg_measure(&pb->w, &all[i]);
        total += all[i].w + (i ? SEP_W : 0);
    }
    int first = 0;
    struct seg more = { .end = 0 };
    strlcpy(more.label, "…", sizeof more.label);
    seg_measure(&pb->w, &more);
    while (total > pb->w.w && first < n - 1) {
        total -= all[first].w + SEP_W;
        if (first == 0)
            total += more.w + SEP_W;
        first++;
    }
    pb->nseg = 0;
    if (first > 0) {
        more.end = all[first - 1].end;
        pb->seg[pb->nseg++] = more;
    }
    for (int i = first; i < n; i++)
        pb->seg[pb->nseg++] = all[i];
    int x = 0;
    for (int i = 0; i < pb->nseg; i++) {
        pb->seg[i].x = x;
        x += pb->seg[i].w + SEP_W;
    }
}

static void pathbar_measure(struct widget *w, struct size_hint *h)
{
    h->pref_h = h->min_h = theme_px(widget_theme(w), TM_CONTROL_H);
    h->pref_w = 200;
    h->min_w = 60;
}

static void pathbar_paint(struct widget *w, struct painter *p)
{
    struct pathbar *pb = (struct pathbar *)w;
    const struct theme *t = p->theme;
    painter_fill(p, 0, 0, w->w, w->h, t->color[TC_WINDOW]);
    pathbar_build(pb);
    int ty = (w->h - painter_text_height(p)) / 2;
    for (int i = 0; i < pb->nseg; i++) {
        struct seg *s = &pb->seg[i];
        if (i > 0)
            painter_text(p, s->x - SEP_W + (SEP_W - painter_text_width(p, "/", 1)) / 2, ty, "/",
                         t->color[TC_TEXT_DISABLED]);
        if (s->current)
            painter_rounded(p, s->x, 0, s->w, w->h, t->color[TC_BUTTON_PRESSED], 0xffffffffu);
        else if (i == pb->hover && s->end)
            painter_rounded(p, s->x, 0, s->w, w->h, t->color[TC_BUTTON_HOVER], 0xffffffffu);
        int x = s->x + SEG_PAD;
        const struct image *icon = s->icon ? icon_get(s->icon) : NULL;
        if (icon) {
            painter_image(p, x, (w->h - image_lh(icon)) / 2, icon);
            x += image_lw(icon) + 6;
        }
        if (s->label[0])
            painter_text(p, x, ty, s->label, t->color[TC_TEXT]);
    }
}

static int seg_at(struct pathbar *pb, int x)
{
    for (int i = 0; i < pb->nseg; i++)
        if (x >= pb->seg[i].x && x < pb->seg[i].x + pb->seg[i].w)
            return i;
    return -1;
}

static int pathbar_event(struct widget *w, struct event *e)
{
    struct pathbar *pb = (struct pathbar *)w;
    switch (e->type) {
    case EV_MOUSE_DOWN: {
        if (!(e->button & 1))
            return 0;
        int i = seg_at(pb, e->x);
        if (i >= 0 && pb->seg[i].end) {
            char path[PATH_MAX];
            strlcpy(path, pb->fv->crumb, sizeof path);
            path[pb->seg[i].end] = '\0';
            folderview_navigate(pb->fv, path);
        }
        return 1;
    }
    case EV_MOUSE_MOVE: {
        int i = seg_at(pb, e->x);
        if (i != pb->hover) {
            pb->hover = i;
            widget_invalidate(w);
        }
        return 1;
    }
    case EV_LEAVE:
        pb->hover = -1;
        widget_invalidate(w);
        return 1;
    default:
        return 0;
    }
}

static const struct widget_class pathbar_class = {
    "pathbar", sizeof(struct pathbar), pathbar_measure, NULL, pathbar_paint, pathbar_event, NULL,
};

/* ---- construction and accessors ---- */

struct folderview *folderview_new(struct widget *bar, struct widget *split, const char *title,
                                  const struct folderview_ops *ops, void *arg)
{
    struct folderview *fv = calloc(1, sizeof *fv);
    if (!fv)
        return NULL;
    fv->app = bar->app;
    fv->win = bar->window;
    fv->ops = ops;
    fv->arg = arg;
    fv->current_place = -1;
    strlcpy(fv->title, title ? title : "", sizeof fv->title);
    home_of(fv->home, sizeof fv->home);
    strlcpy(fv->cwd, fv->home, sizeof fv->cwd);
    strlcpy(fv->crumb, fv->home, sizeof fv->crumb);
    build_places(fv);

    struct pathbar *pb = (struct pathbar *)widget_new(&pathbar_class, bar);
    pb->fv = fv;
    pb->hover = -1;
    fv->pathbar = &pb->w;
    widget_set_stretch(fv->pathbar, 1, 0);
    widget_set_id(fv->pathbar, "fv-pathbar");
    fv->location = textfield_new(bar, "");
    widget_set_id(fv->location, "fv-location");
    widget_connect(fv->location, "changed", on_location_changed, fv);
    widget_connect(fv->location, "activate", on_location_activate, fv);
    fv->search = textfield_new(bar, "");
    widget_set_id(fv->search, "fv-search");
    widget_connect(fv->search, "changed", on_search_changed, fv);
    widget_connect(fv->search, "activate", on_search_activate, fv);
    fv->search_button = button_new(bar, "");
    widget_set_icon(fv->search_button, icon_get("search"));
    widget_set_tip(fv->search_button, _("Search"));
    int h = theme_px(widget_theme(fv->search_button), TM_CONTROL_H);
    widget_set_hint(fv->search_button, h + 6, h);
    widget_connect(fv->search_button, "clicked", on_search_button, fv);
    show_bar(fv, fv->pathbar);

    struct sidebar *sb = (struct sidebar *)widget_new(&sidebar_class, split);
    sb->fv = fv;
    sb->hover = -1;
    fv->sidebar = &sb->w;
    fv->sidebar->focusable = 1;
    widget_set_id(fv->sidebar, "fv-sidebar");
    fv->table = table_new(split);
    widget_set_id(fv->table, "fv-table");
    fv->model = (struct model){ m_rows, m_child, m_columns, m_cell, m_header, m_sort, fv, m_icon };
    view_set_model(fv->table, &fv->model);
    table_set_column_width(fv->table, 0, 220);
    table_set_column_width(fv->table, 1, 90);
    table_set_column_width(fv->table, 2, 100);
    table_set_column_width(fv->table, 3, 110);
    widget_connect(fv->table, "activate", on_activate, fv);
    widget_connect(fv->table, "selected", on_selected, fv);
    splitpane_set_position(split, SIDEBAR_W);
    widget_connect(fv->win, "key", on_key, fv);

    reload(fv);
    fv->ready = 1;
    fv->timer = app_timer_add(fv->app, POLL_MS, 1, poll, fv);
    return fv;
}

struct widget *folderview_table(struct folderview *fv) { return fv->table; }
struct widget *folderview_sidebar(struct folderview *fv) { return fv->sidebar; }
enum folderview_kind folderview_kind(const struct folderview *fv) { return fv->view; }
const char *folderview_cwd(const struct folderview *fv) { return fv->cwd; }
const char *folderview_home_path(const struct folderview *fv) { return fv->home; }
int folderview_count(const struct folderview *fv) { return fv->nentries; }
int folderview_hidden_count(const struct folderview *fv) { return fv->nhidden; }
int folderview_hidden(const struct folderview *fv) { return fv->show_hidden; }

const struct folderview_entry *folderview_selected(const struct folderview *fv)
{
    int row = fv->table->value;
    return row >= 0 && row < fv->nentries ? &fv->entries[row] : NULL;
}

void folderview_set_hidden(struct folderview *fv, int show)
{
    fv->show_hidden = show != 0;
    folderview_reload(fv);
}

void folderview_sort(struct folderview *fv, int column, int descending)
{
    fv->sort_col = column;
    fv->sort_desc = descending;
    folderview_reload(fv);
}
