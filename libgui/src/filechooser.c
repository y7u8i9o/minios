/* The file chooser, a modal window modelled on the GNOME one.  Around the
 * folder view that the file manager shows as well (folderview.c) it adds
 * file type filters, Open or Save and Cancel, and in save mode a name field,
 * a New folder button and a confirmation before an existing file is
 * replaced.  Chosen files are recorded in the recent list. */
#include <gui/app.h>
#include <gui/folderview.h>
#include <gui/mime.h>
#include <errno.h>
#include <fnmatch.h>
#include <libintl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "dialog.h"
#include "filechooser.h"

/* tools/xgettext.py extracts these into the libgui domain. */
#define _(s) dgettext("libgui", s)

#ifndef PATH_MAX
#define PATH_MAX 512
#endif

#define WINDOW_W 760
#define WINDOW_H 500

struct chooser {
    struct app *app;
    struct folderview *fv;
    struct widget *win, *name, *filter, *accept, *menu, *visit_item, *hidden_item;
    enum file_chooser_mode mode;
    const struct file_filter *filters;
    int nfilters;
    int state;                  /* 0 open, 1 chosen, -1 cancelled */
    char title[128];
    char result[PATH_MAX];
};

static void message(struct chooser *c, const char *text)
{
    const char *const buttons[] = { _("OK") };
    dialog_message(c->app, c->win, c->title, text, buttons, 1);
}

static int is_dir(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static void update_accept(struct chooser *c)
{
    int on;
    if (c->mode == FILE_CHOOSER_SAVE) {
        on = widget_text(c->name)[0] != '\0';
    } else {
        struct widget *loc = widget_find(c->win, "fv-location");
        on = folderview_selected(c->fv) != NULL || (loc->visible && widget_text(loc)[0]);
    }
    widget_set_enabled(c->accept, on);
}

static void finish(struct chooser *c, const char *path)
{
    strlcpy(c->result, path, sizeof c->result);
    folderview_recent_add(path);
    c->state = 1;
}

/* In save mode the name is relative to the current folder.  A folder is
 * shown, and an existing file is replaced only after a confirmation. */
static void accept_save(struct chooser *c, const char *text)
{
    if (!text[0])
        return;
    char path[PATH_MAX], dir[PATH_MAX];
    folderview_resolve(c->fv, text, path, sizeof path);
    if (is_dir(path)) {
        folderview_navigate(c->fv, path);
        widget_set_text(c->name, "");
        widget_focus(c->name);
        update_accept(c);
        return;
    }
    strlcpy(dir, path, sizeof dir);
    char *slash = strrchr(dir, '/');
    slash[slash == dir] = '\0';
    if (!is_dir(dir)) {
        char msg[PATH_MAX + 80];
        snprintf(msg, sizeof msg, _("The folder “%s” does not exist."), dir);
        message(c, msg);
        return;
    }
    if (access(path, F_OK) == 0) {
        char msg[PATH_MAX + 120];
        snprintf(msg, sizeof msg, _("A file named “%s” already exists. Do you want to replace it?"),
                 strrchr(path, '/') + 1);
        const char *const buttons[] = { _("Cancel"), _("Replace") };
        if (dialog_message(c->app, c->win, _("Replace file"), msg, buttons, 2) != 1)
            return;
    }
    finish(c, path);
}

/* ---- folder view callbacks ---- */

static void fv_open(struct folderview *fv, const char *path, void *arg)
{
    struct chooser *c = arg;
    if (c->mode == FILE_CHOOSER_OPEN) {
        finish(c, path);
        return;
    }
    char full[PATH_MAX];
    strlcpy(full, path, sizeof full);
    widget_set_text(c->name, strrchr(full, '/') + 1);
    accept_save(c, full);
}

static void fv_changed(struct folderview *fv, void *arg)
{
    update_accept(arg);
}

static void fv_selected(struct folderview *fv, void *arg)
{
    struct chooser *c = arg;
    const struct folderview_entry *e = folderview_selected(fv);
    if (c->mode == FILE_CHOOSER_SAVE && e && !e->dir)
        widget_set_text(c->name, e->name);
    update_accept(c);
}

/* A filter's patterns are separated by spaces.  One with a slash matches
 * the MIME type of the name, the others the name itself. */
static int fv_filter(struct folderview *fv, const char *name, void *arg)
{
    struct chooser *c = arg;
    if (!c->nfilters || c->filter->value < 0 || c->filter->value >= c->nfilters)
        return 1;
    const char *p = c->filters[c->filter->value].patterns;
    char pat[64];
    while (p && *p) {
        while (*p == ' ')
            p++;
        size_t n = strcspn(p, " ");
        if (n == 0)
            break;
        if (n < sizeof pat) {
            memcpy(pat, p, n);
            pat[n] = '\0';
            if (strchr(pat, '/') ? fnmatch(pat, mime_type(name, 0), 0) == 0 : fnmatch(pat, name, FNM_CASEFOLD) == 0)
                return 1;
        }
        p += n;
    }
    return 0;
}

/* In save mode a path is typed into the name field. */
static int fv_key(struct folderview *fv, const struct sig_key *k, void *arg)
{
    struct chooser *c = arg;
    if (c->mode != FILE_CHOOSER_SAVE)
        return 0;
    int ctrl = k->mods & WMOD_CTRL, alt = k->mods & WMOD_ALT;
    if (ctrl && k->code == KEY_L) {
        widget_focus(c->name);
        textfield_select(c->name, 0, -1);
        return 1;
    }
    struct widget *focus = widget_focused(c->win);
    if ((k->ch == '/' || k->ch == '~') && !ctrl && !alt &&
        (focus == folderview_table(fv) || focus == folderview_sidebar(fv))) {
        char s[2] = { (char)k->ch, '\0' };
        widget_set_text(c->name, s);
        textfield_select(c->name, -1, -1);
        widget_focus(c->name);
        return 1;
    }
    return 0;
}

static const struct folderview_ops chooser_ops = { fv_open, fv_changed, fv_selected, fv_filter, fv_key };

/* ---- handlers ---- */

static int on_accept(struct widget *w, void *args, void *arg)
{
    struct chooser *c = arg;
    if (c->mode == FILE_CHOOSER_SAVE) {
        accept_save(c, widget_text(c->name));
        return 1;
    }
    struct widget *loc = widget_find(c->win, "fv-location");
    if (loc->visible && widget_text(loc)[0]) {
        struct sig_change s = { 0, widget_text(loc) };
        widget_emit(loc, "activate", &s);
        return 1;
    }
    const struct folderview_entry *e = folderview_selected(c->fv);
    if (!e)
        return 1;
    char path[PATH_MAX];
    strlcpy(path, e->path, sizeof path);
    if (e->dir)
        folderview_navigate(c->fv, path);
    else
        finish(c, path);
    return 1;
}

static int on_cancel(struct widget *w, void *args, void *arg)
{
    ((struct chooser *)arg)->state = -1;
    return 1;
}

static int on_name_activate(struct widget *w, void *args, void *arg)
{
    struct chooser *c = arg;
    accept_save(c, widget_text(c->name));
    return 1;
}

static int on_name_changed(struct widget *w, void *args, void *arg)
{
    update_accept(arg);
    return 0;
}

static int on_filter(struct widget *w, void *args, void *arg)
{
    folderview_reload(((struct chooser *)arg)->fv);
    return 1;
}

static int on_new_folder(struct widget *w, void *args, void *arg)
{
    struct chooser *c = arg;
    char name[NAME_MAX + 1] = "";
    if (!dialog_prompt(c->app, c->win, _("New folder"), _("Folder name:"), name, sizeof name) || !name[0])
        return 1;
    char path[PATH_MAX], msg[PATH_MAX + 80];
    if (strchr(name, '/')) {
        snprintf(msg, sizeof msg, _("A folder name cannot contain “/”."));
        message(c, msg);
        return 1;
    }
    folderview_resolve(c->fv, name, path, sizeof path);
    if (mkdir(path, 0755) < 0) {
        snprintf(msg, sizeof msg, _("The folder “%s” cannot be created: %s."), name, strerror(errno));
        message(c, msg);
        return 1;
    }
    folderview_navigate(c->fv, path);
    return 1;
}

static int on_copy_location(struct widget *w, void *args, void *arg)
{
    struct chooser *c = arg;
    const struct folderview_entry *e = folderview_selected(c->fv);
    const char *p = e ? e->path : folderview_cwd(c->fv);
    gui_clipboard_set(p, (int)strlen(p));
    return 1;
}

static int on_visit(struct widget *w, void *args, void *arg)
{
    folderview_visit(((struct chooser *)arg)->fv);
    return 1;
}

static int on_hidden(struct widget *w, void *args, void *arg)
{
    struct chooser *c = arg;
    folderview_set_hidden(c->fv, !folderview_hidden(c->fv));
    return 1;
}

static int on_context(struct widget *w, void *args, void *arg)
{
    struct chooser *c = arg;
    struct sig_click *k = args;
    widget_set_visible(c->visit_item, folderview_kind(c->fv) != FOLDERVIEW_FOLDER && folderview_selected(c->fv));
    widget_set_text(c->hidden_item, folderview_hidden(c->fv) ? _("Hide hidden files") : _("Show hidden files"));
    int ax, ay;
    widget_abs(w, &ax, &ay);
    menu_popup(c->menu, ax + k->x, ay + k->y);
    return 1;
}

/* Escape, when the folder view did not take it for its search or its
 * location entry, cancels. */
static int on_key(struct widget *w, void *args, void *arg)
{
    struct sig_key *k = args;
    if (k->code != KEY_ESC)
        return 0;
    ((struct chooser *)arg)->state = -1;
    return 1;
}

static int on_close(struct widget *w, void *args, void *arg)
{
    ((struct chooser *)arg)->state = -1;
    return 1;
}

/* ---- construction ---- */

static void build(struct chooser *c)
{
    struct widget *win = c->win;
    widget_connect(win, "close", on_close, c);
    if (c->mode == FILE_CHOOSER_SAVE) {
        struct widget *row = box_new(win, 0);
        label_new(row, _("Name:"));
        c->name = textfield_new(row, "");
        widget_set_id(c->name, "fc-name");
        widget_connect(c->name, "activate", on_name_activate, c);
        widget_connect(c->name, "changed", on_name_changed, c);
    }
    struct widget *bar = box_new(win, 0);
    struct widget *split = splitpane_new(win, 0);
    struct widget *row = box_new(win, 0);
    c->filter = combobox_new(row);
    widget_set_id(c->filter, "fc-filter");
    for (int i = 0; i < c->nfilters; i++)
        combobox_add(c->filter, c->filters[i].name);
    combobox_select(c->filter, 0);
    widget_set_visible(c->filter, c->nfilters > 0);
    widget_connect(c->filter, "changed", on_filter, c);
    widget_set_stretch(label_new(row, ""), 1, 0);
    struct widget *cancel = button_new(row, _("Cancel"));
    widget_set_id(cancel, "fc-cancel");
    widget_connect(cancel, "clicked", on_cancel, c);
    c->accept = button_new(row, c->mode == FILE_CHOOSER_SAVE ? _("Save") : _("Open"));
    widget_set_id(c->accept, "fc-accept");
    widget_connect(c->accept, "clicked", on_accept, c);
    int bw = theme_px(app_theme(c->app), TM_CONTROL_H) * 4;
    widget_set_hint(cancel, bw, 0);
    widget_set_hint(c->accept, bw, 0);

    c->fv = folderview_new(bar, split, c->title, &chooser_ops, c);
    if (!c->fv)
        return;
    widget_connect(win, "key", on_key, c);
    if (c->mode == FILE_CHOOSER_SAVE) {
        struct widget *b = button_new(bar, "");
        widget_set_icon(b, icon_get("folder-new"));
        widget_set_tip(b, _("New folder"));
        int h = theme_px(widget_theme(b), TM_CONTROL_H);
        widget_set_hint(b, h + 6, h);
        widget_set_id(b, "fc-new-folder");
        widget_connect(b, "clicked", on_new_folder, c);
    }
    struct widget *table = folderview_table(c->fv);
    widget_connect(table, "context", on_context, c);

    c->menu = popupmenu_new(win);
    c->visit_item = menu_add(c->menu, _("Visit file"), "open");
    widget_connect(c->visit_item, "clicked", on_visit, c);
    widget_connect(menu_add(c->menu, _("Copy location"), "copy"), "clicked", on_copy_location, c);
    menu_add_separator(c->menu);
    c->hidden_item = menu_add(c->menu, _("Show hidden files"), NULL);
    widget_connect(c->hidden_item, "clicked", on_hidden, c);
}

/* The initial folder and name come from the path given by the program,
 * which is a folder, a file in a folder, or a name whose folder does not
 * exist yet and falls back to the nearest existing folder above it. */
static void start(struct chooser *c, const char *initial)
{
    char dir[PATH_MAX], name[NAME_MAX + 1] = "";
    if (!initial || !initial[0]) {
        strlcpy(dir, folderview_home_path(c->fv), sizeof dir);
    } else {
        folderview_resolve(c->fv, initial, dir, sizeof dir);
        if (initial[strlen(initial) - 1] != '/' && !is_dir(dir)) {
            char *slash = strrchr(dir, '/');
            strlcpy(name, slash + 1, sizeof name);
            slash[slash == dir] = '\0';
        }
    }
    while (!is_dir(dir) && strcmp(dir, "/") != 0) {
        char *slash = strrchr(dir, '/');
        slash[slash == dir] = '\0';
    }
    folderview_navigate(c->fv, dir);
    if (c->mode == FILE_CHOOSER_SAVE) {
        widget_set_text(c->name, name);
        /* The name is selected without its extension, as in GNOME. */
        const char *dot = strrchr(name, '.');
        textfield_select(c->name, 0, dot && dot != name ? (int)(dot - name) : -1);
        widget_focus(c->name);
    } else {
        if (name[0])
            folderview_select_name(c->fv, name);
        widget_focus(folderview_table(c->fv));
    }
    update_accept(c);
}

struct chooser *chooser_open(struct app *a, struct widget *parent, enum file_chooser_mode mode, const char *title,
                             const struct file_filter *filters, int nfilters, const char *path)
{
    struct chooser *c = calloc(1, sizeof *c);
    if (!c)
        return NULL;
    c->app = a;
    c->mode = mode;
    c->filters = filters;
    c->nfilters = filters ? nfilters : 0;
    if (!title)
        title = mode == FILE_CHOOSER_SAVE ? _("Save file") : _("Open file");
    strlcpy(c->title, title, sizeof c->title);
    int w = WINDOW_W, h = WINDOW_H;
    if (gui_screen_width() > 0 && w > gui_screen_width() - 40)
        w = gui_screen_width() - 40;
    if (gui_screen_height() > 0 && h > gui_screen_height() - 80)
        h = gui_screen_height() - 80;
    c->win = app_modal_window(a, parent, w, h, title);
    if (!c->win) {
        free(c);
        return NULL;
    }
    build(c);
    if (!c->fv) {
        window_close(c->win);
        app_step(a, 0);
        free(c);
        return NULL;
    }
    start(c, path);
    return c;
}

struct widget *chooser_window(struct chooser *c) { return c->win; }

int chooser_state(struct chooser *c, char *path, int size)
{
    if (c->state == 1 && path)
        strlcpy(path, c->result, (size_t)size);
    return c->state;
}

/* The folder view goes with the window's widgets. */
void chooser_close(struct chooser *c)
{
    window_close(c->win);
    app_step(c->app, 0);
    free(c);
}

int app_choose_file(struct app *a, enum file_chooser_mode mode, const char *title, const struct file_filter *filters,
                    int nfilters, char *path, int size)
{
    struct chooser *c = chooser_open(a, app_first_window(a), mode, title, filters, nfilters, path);
    if (!c)
        return 0;
    int state;
    while ((state = chooser_state(c, path, size)) == 0 && app_step(a, -1))
        ;
    chooser_close(c);
    return state == 1;
}
