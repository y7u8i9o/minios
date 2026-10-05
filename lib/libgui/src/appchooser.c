/* The application chooser for Open with, a modal window modelled on the
 * GNOME one.  A tree lists the applications of the launcher tables in two
 * groups.  The group Recommended applications contains the handlers of
 * the file's type in the order of mime_handlers.  The group Other
 * applications contains the remaining entries in the order of their
 * titles.  A handler without a launcher entry appears under the file name
 * of its program.  Entries with an "@" command are actions of the panel
 * and are left out.  The check box Always use makes the chosen command
 * the handler of the type in the user's handler table.  The button Other
 * program opens the file chooser in /usr/bin and adds the chosen program
 * to the top of Other applications. */
#include <gui/app.h>
#include <gui/launcher.h>
#include <gui/mime.h>
#include <gui/model.h>
#include <errno.h>
#include <libintl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>
#include "appchooser.h"
#include "dialog.h"
#include "filechooser.h"

/* tools/xgettext.py extracts these into the libgui domain. */
#define _(s) dgettext("libgui", s)

#ifndef PATH_MAX
#define PATH_MAX 512
#endif

#define WINDOW_W 440
#define WINDOW_H 480
#define APPS_MAX 96
#define HANDLERS_MAX 16
#define PROGRAM_DIR "/usr/bin/"

/* The tree rows: the two groups, then one row per application.  The
 * application at index i of apps has the row id ROW_APP + i. */
enum { ROW_RECOMMENDED, ROW_OTHER, ROW_APP };

struct appchooser {
    struct app *app;
    struct widget *win, *tree, *always, *accept;
    struct model model;
    /* apps contains the recommended entries first, then the others. */
    struct launcher_entry apps[APPS_MAX];
    int napps, nrecommended;
    int state;                  /* 0 open, 1 chosen, -1 cancelled */
    char path[PATH_MAX];
    char type[64];
    char result[LAUNCHER_COMMAND];
};

static void message(struct appchooser *c, const char *text)
{
    const char *const buttons[] = { _("OK") };
    dialog_message(c->app, c->win, _("Open with"), text, buttons, 1);
}

static const char *base_name(const char *path)
{
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

/* ---- the list of applications ---- */

static int same_program(const char *a, const char *b)
{
    char pa[LAUNCHER_COMMAND], pb[LAUNCHER_COMMAND];
    return strcmp(launcher_program(a, pa, sizeof pa), launcher_program(b, pb, sizeof pb)) == 0;
}

static int compare_titles(const void *a, const void *b)
{
    const struct launcher_entry *x = a, *y = b;
    return strcasecmp(x->title, y->title);
}

/* load fills apps from the launcher tables and the handlers of the type.
 * An exact command match between a handler and a launcher entry wins
 * over a match of the program alone. */
static void load(struct appchooser *c)
{
    static struct launcher_entry table[APPS_MAX];
    char path[300];
    int n = launcher_read_apps(table, APPS_MAX);
    n = launcher_read_table(launcher_system_path(path, sizeof path), 1, table, n, APPS_MAX);
    char used[APPS_MAX] = { 0 };
    for (int i = 0; i < n; i++) {
        if (table[i].command[0] == '@' || !table[i].command[0])
            used[i] = 1;
        for (int j = 0; j < i && !used[i]; j++)
            if (!used[j] && strcmp(table[i].command, table[j].command) == 0)
                used[i] = 1;
    }

    const char *handlers[HANDLERS_MAX];
    int nh = mime_handlers(c->type, handlers, HANDLERS_MAX);
    c->napps = 0;
    for (int h = 0; h < nh && c->napps < APPS_MAX; h++) {
        int found = -1;
        for (int i = 0; i < n && found < 0; i++)
            if (!used[i] && strcmp(table[i].command, handlers[h]) == 0)
                found = i;
        for (int i = 0; i < n && found < 0; i++)
            if (!used[i] && same_program(table[i].command, handlers[h]))
                found = i;
        struct launcher_entry *e = &c->apps[c->napps++];
        if (found >= 0) {
            used[found] = 1;
            strlcpy(e->title, table[found].title, sizeof e->title);
        } else {
            char program[LAUNCHER_COMMAND];
            strlcpy(e->title, base_name(launcher_program(handlers[h], program, sizeof program)), sizeof e->title);
        }
        strlcpy(e->command, handlers[h], sizeof e->command);
    }
    c->nrecommended = c->napps;
    for (int i = 0; i < n && c->napps < APPS_MAX; i++)
        if (!used[i])
            c->apps[c->napps++] = table[i];
    qsort(c->apps + c->nrecommended, (size_t)(c->napps - c->nrecommended), sizeof c->apps[0], compare_titles);
}

/* ---- the tree model ---- */

static int group_size(struct appchooser *c, int group)
{
    return group == ROW_RECOMMENDED ? c->nrecommended : c->napps - c->nrecommended;
}

static int m_rows(struct model *m, int parent)
{
    struct appchooser *c = m->user;
    if (parent < 0)
        return (c->nrecommended > 0) + (c->napps > c->nrecommended);
    return parent < ROW_APP ? group_size(c, parent) : 0;
}

static int m_child(struct model *m, int parent, int index)
{
    struct appchooser *c = m->user;
    if (parent < 0)
        return index == 0 && c->nrecommended > 0 ? ROW_RECOMMENDED : ROW_OTHER;
    return ROW_APP + (parent == ROW_RECOMMENDED ? 0 : c->nrecommended) + index;
}

static int m_columns(struct model *m) { return 1; }

static const char *m_cell(struct model *m, int row, int col, char *buf, size_t size)
{
    struct appchooser *c = m->user;
    if (row == ROW_RECOMMENDED)
        return _("Recommended applications");
    if (row == ROW_OTHER)
        return _("Other applications");
    return c->apps[row - ROW_APP].title;
}

/* An application has the icon app-NAME, where NAME is the file name of
 * its program, as in the launcher menu of the panel. */
static const struct image *m_icon(struct model *m, int row)
{
    struct appchooser *c = m->user;
    if (row < ROW_APP)
        return NULL;
    char program[LAUNCHER_COMMAND], name[64];
    launcher_program(c->apps[row - ROW_APP].command, program, sizeof program);
    snprintf(name, sizeof name, "app-%s", base_name(program));
    const struct image *img = icon_get(name);
    return img ? img : icon_get("app-default");
}

/* ---- choosing ---- */

static int selected_app(struct appchooser *c)
{
    int row = c->tree->value;
    return row >= ROW_APP && row < ROW_APP + c->napps ? row - ROW_APP : -1;
}

static void update_accept(struct appchooser *c)
{
    widget_set_enabled(c->accept, selected_app(c) >= 0);
}

/* finish records the command of the application at index i.  With
 * Always use checked, the command becomes the handler of the type. */
static void finish(struct appchooser *c, int i)
{
    strlcpy(c->result, c->apps[i].command, sizeof c->result);
    if (c->always->value) {
        mime_set_handler(c->type, c->result);
        int r = mime_save(NULL);
        if (r < 0) {
            char text[256];
            snprintf(text, sizeof text, _("The default application cannot be saved: %s."), strerror(-r));
            message(c, text);
        }
    }
    c->state = 1;
}

static int is_program(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode) && access(path, X_OK) == 0;
}

int appchooser_add_program(struct appchooser *c, const char *path)
{
    if (!is_program(path))
        return -ENOEXEC;
    int i;
    for (i = 0; i < c->napps; i++)
        if (strcmp(c->apps[i].command, path) == 0)
            break;
    if (i == c->napps) {
        if (c->napps == APPS_MAX)
            c->napps--;
        i = c->nrecommended;
        memmove(&c->apps[i + 1], &c->apps[i], (size_t)(c->napps - i) * sizeof c->apps[0]);
        c->napps++;
        strlcpy(c->apps[i].title, base_name(path), sizeof c->apps[i].title);
        strlcpy(c->apps[i].command, path, sizeof c->apps[i].command);
        view_refresh(c->tree);
    }
    treeview_expand(c->tree, i < c->nrecommended ? ROW_RECOMMENDED : ROW_OTHER, 1);
    view_select(c->tree, ROW_APP + i);
    update_accept(c);
    return 0;
}

/* ---- handlers ---- */

static int on_selected(struct widget *w, void *args, void *arg)
{
    update_accept(arg);
    return 1;
}

static int on_activate(struct widget *w, void *args, void *arg)
{
    struct appchooser *c = arg;
    int i = selected_app(c);
    if (i >= 0)
        finish(c, i);
    return 1;
}

static int on_accept(struct widget *w, void *args, void *arg)
{
    return on_activate(w, args, arg);
}

static int on_other(struct widget *w, void *args, void *arg)
{
    struct appchooser *c = arg;
    struct chooser *fc = chooser_open(c->app, c->win, FILE_CHOOSER_OPEN, _("Choose a program"), NULL, 0, PROGRAM_DIR);
    if (!fc)
        return 1;
    char path[PATH_MAX];
    int state;
    while ((state = chooser_state(fc, path, sizeof path)) == 0 && app_step(c->app, -1))
        ;
    chooser_close(fc);
    if (state == 1 && appchooser_add_program(c, path) < 0) {
        char text[PATH_MAX + 80];
        snprintf(text, sizeof text, _("“%s” is not a program."), base_name(path));
        message(c, text);
    }
    return 1;
}

static int on_cancel(struct widget *w, void *args, void *arg)
{
    ((struct appchooser *)arg)->state = -1;
    return 1;
}

static int on_key(struct widget *w, void *args, void *arg)
{
    struct sig_key *k = args;
    if (k->code != KEY_ESC)
        return 0;
    ((struct appchooser *)arg)->state = -1;
    return 1;
}

/* ---- construction ---- */

static void build(struct appchooser *c)
{
    struct widget *win = c->win;
    char text[PATH_MAX + 80];
    snprintf(text, sizeof text, _("Choose an application to open “%s”."), base_name(c->path));
    label_new(win, text);

    c->model = (struct model){ m_rows, m_child, m_columns, m_cell, NULL, NULL, c, m_icon };
    c->tree = treeview_new(win);
    widget_set_id(c->tree, "ac-tree");
    widget_set_stretch(c->tree, 1, 1);
    view_set_model(c->tree, &c->model);
    treeview_expand(c->tree, ROW_RECOMMENDED, 1);
    treeview_expand(c->tree, ROW_OTHER, 1);
    widget_connect(c->tree, "selected", on_selected, c);
    widget_connect(c->tree, "activate", on_activate, c);

    snprintf(text, sizeof text, _("Always use for files of type %s"), c->type);
    c->always = checkbox_new(win, text);
    widget_set_id(c->always, "ac-always");

    struct widget *row = box_new(win, 0);
    struct widget *other = button_new(row, _("Other program..."));
    widget_set_id(other, "ac-other");
    widget_connect(other, "clicked", on_other, c);
    widget_set_stretch(label_new(row, ""), 1, 0);
    struct widget *cancel = button_new(row, _("Cancel"));
    widget_set_id(cancel, "ac-cancel");
    widget_connect(cancel, "clicked", on_cancel, c);
    c->accept = button_new(row, _("Open"));
    widget_set_id(c->accept, "ac-accept");
    widget_connect(c->accept, "clicked", on_accept, c);
    int bw = theme_px(app_theme(c->app), TM_CONTROL_H) * 4;
    widget_set_hint(cancel, bw, 0);
    widget_set_hint(c->accept, bw, 0);

    widget_connect(win, "key", on_key, c);
    widget_connect(win, "close", on_cancel, c);
}

struct appchooser *appchooser_open(struct app *a, struct widget *parent, const char *path)
{
    struct appchooser *c = calloc(1, sizeof *c);
    if (!c)
        return NULL;
    c->app = a;
    strlcpy(c->path, path, sizeof c->path);
    /* Packages may have been installed or removed since this window opened. */
    mime_load(NULL, NULL);
    struct stat st;
    strlcpy(c->type, mime_type(path, stat(path, &st) == 0 && S_ISDIR(st.st_mode)), sizeof c->type);
    load(c);
    int w = WINDOW_W, h = WINDOW_H;
    if (gui_screen_width() > 0 && w > gui_screen_width() - 40)
        w = gui_screen_width() - 40;
    if (gui_screen_height() > 0 && h > gui_screen_height() - 80)
        h = gui_screen_height() - 80;
    c->win = app_modal_window(a, parent, w, h, _("Open with"));
    if (!c->win) {
        free(c);
        return NULL;
    }
    build(c);
    if (c->nrecommended > 0)
        view_select(c->tree, ROW_APP);
    widget_focus(c->tree);
    update_accept(c);
    return c;
}

struct widget *appchooser_window(struct appchooser *c) { return c->win; }

int appchooser_state(struct appchooser *c, char *command, int size)
{
    if (c->state == 1 && command)
        strlcpy(command, c->result, (size_t)size);
    return c->state;
}

void appchooser_close(struct appchooser *c)
{
    window_close(c->win);
    app_step(c->app, 0);
    free(c);
}

int app_choose_program(struct app *a, const char *path, char *command, int size)
{
    struct appchooser *c = appchooser_open(a, app_first_window(a), path);
    if (!c)
        return 0;
    int state;
    while ((state = appchooser_state(c, command, size)) == 0 && app_step(a, -1))
        ;
    appchooser_close(c);
    return state == 1;
}
