/* files: a directory table (name, size, type) over a model, a path
 * field, a tool bar and a status bar. Enter or the Open button enters a
 * directory or opens a file in the viewer. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <gui/app.h>
#include <gui/mime.h>
#include <gui/model.h>

struct entry {
    char name[64];
    long size;
    int dir;
};

static struct app *app;
static struct widget *path_field, *table, *status;
static char cwd[256] = "/";
static struct entry *entries;
static int nentries, sort_desc;

static int m_rows(struct model *m, int parent) { return parent < 0 ? nentries : 0; }
static int m_child(struct model *m, int parent, int index) { return index; }
static int m_columns(struct model *m) { return 3; }
static const char *m_cell(struct model *m, int row, int col, char *buf, size_t size)
{
    struct entry *e = &entries[row];
    if (col == 0)
        return e->name;
    if (col == 1) {
        if (e->dir)
            return "";
        snprintf(buf, size, "%ld", e->size);
        return buf;
    }
    return e->dir ? "directory" : "file";
}
static const char *m_header(struct model *m, int col) { return col == 0 ? "Name" : col == 1 ? "Size" : "Type"; }

static int cmp(const void *a, const void *b)
{
    const struct entry *x = a, *y = b;
    if (x->dir != y->dir)
        return y->dir - x->dir;
    int r = strcmp(x->name, y->name);
    return sort_desc ? -r : r;
}

static void m_sort(struct model *m, int col, int desc)
{
    sort_desc = desc;
    qsort(entries, (size_t)nentries, sizeof *entries, cmp);
}

static struct model model = { m_rows, m_child, m_columns, m_cell, m_header, m_sort, NULL };

static void refresh(void)
{
    widget_set_text(path_field, cwd);
    free(entries);
    entries = NULL;
    nentries = 0;
    DIR *d = opendir(cwd);
    if (!d) {
        widget_set_text(status, "cannot open directory");
        view_refresh(table);
        return;
    }
    struct dirent *e;
    while ((e = readdir(d))) {
        if (strcmp(e->d_name, ".") == 0)
            continue;
        entries = realloc(entries, (size_t)(nentries + 1) * sizeof *entries);
        struct entry *en = &entries[nentries++];
        strlcpy(en->name, e->d_name, sizeof en->name);
        en->dir = e->d_type == DT_DIR;
        char path[512];
        snprintf(path, sizeof path, "%s%s%s", cwd, strcmp(cwd, "/") == 0 ? "" : "/", e->d_name);
        struct stat st;
        en->size = stat(path, &st) == 0 ? (long)st.st_size : 0;
    }
    closedir(d);
    qsort(entries, (size_t)nentries, sizeof *entries, cmp);
    view_refresh(table);
    char msg[64];
    snprintf(msg, sizeof msg, "%d entries", nentries);
    widget_set_text(status, msg);
}

static void enter(const char *name)
{
    char path[512];
    if (strcmp(name, "..") == 0) {
        char *slash = strrchr(cwd, '/');
        if (slash && slash != cwd)
            *slash = '\0';
        else
            strcpy(cwd, "/");
    } else {
        snprintf(path, sizeof path, "%s%s%s", cwd, strcmp(cwd, "/") == 0 ? "" : "/", name);
        strlcpy(cwd, path, sizeof cwd);
    }
    refresh();
}

static void open_selected(void)
{
    int row = table->value;
    if (row < 0 || row >= nentries)
        return;
    struct entry *e = &entries[row];
    if (e->dir) {
        enter(e->name);
        return;
    }
    char path[512];
    snprintf(path, sizeof path, "%s%s%s", cwd, strcmp(cwd, "/") == 0 ? "" : "/", e->name);
    printf("files: open %s\n", path);
    fflush(stdout);
    pid_t pid = mime_open(path);
    if (pid < 0) {
        static const char *const buttons[] = { "OK" };
        app_dialog(app, "Open", "No program is registered for this file type.", buttons, 1);
    }
}

static int on_activate(struct widget *w, void *args, void *arg) { open_selected(); return 1; }
static int on_open(struct widget *w, void *args, void *arg) { open_selected(); return 1; }
static int on_up(struct widget *w, void *args, void *arg) { enter(".."); return 1; }
static int on_go(struct widget *w, void *args, void *arg)
{
    strlcpy(cwd, widget_text(path_field), sizeof cwd);
    refresh();
    return 1;
}

int main(int argc, char **argv)
{
    if (argc > 1)
        strlcpy(cwd, argv[1], sizeof cwd);
    app = app_create();
    if (!app)
        return 1;
    struct widget *win = app_window(app, 420, 320, "files");
    if (!win)
        return 1;
    struct widget *bar = toolbar_new(win);
    widget_connect(toolbar_add(bar, "up", "Parent directory"), "clicked", on_up, NULL);
    widget_connect(toolbar_add(bar, "open", "Open"), "clicked", on_open, NULL);
    path_field = textfield_new(bar, cwd);
    widget_connect(path_field, "activate", on_go, NULL);
    table = table_new(win);
    view_set_model(table, &model);
    table_set_column_width(table, 0, 200);
    table_set_column_width(table, 1, 80);
    widget_connect(table, "activate", on_activate, NULL);
    struct widget *sb = statusbar_new(win);
    status = statusbar_add(sb, 1);
    refresh();
    widget_focus(table);
    app_run(app);
    app_destroy(app);
    while (waitpid(-1, NULL, WNOHANG) > 0)
        ;
    return 0;
}
