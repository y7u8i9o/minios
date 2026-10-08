/* files is the file manager window, built like GNOME Files around the
 * folder view that the file chooser of libgui shows as well
 * (gui/folderview.h), which contains the places sidebar, the path bar with
 * the location entry and the search, and the table of the folder.  Around
 * it are a menu bar with the file operations of fsops.c, Back and
 * Forward, a context menu and a status bar.
 *
 *   files [directory] */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <sys/stat.h>
#include <gui/app.h>
#include <gui/fileops.h>
#include <gui/folderview.h>
#include <gui/i18n.h>
#include <gui/mime.h>
#include "files.h"

#define HISTORY 32

static struct app *app;
static struct folderview *fv;
static struct widget *win, *status, *sel_label;
static struct widget *item_menu, *dir_menu, *location_item, *back_button, *forward_button;
static char history[HISTORY][512];
static int hist_len, hist_pos;
static char clip_path[512];
static int clip_cut;

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

static const struct folderview_entry *selected_entry(void)
{
    return folderview_selected(fv);
}

/* New entries go to the current folder. */
static const char *cwd(void)
{
    return folderview_cwd(fv);
}

/* ---- folder view callbacks ---- */

static void update_selection_label(void)
{
    const struct folderview_entry *e = selected_entry();
    char text[320], size[32];
    if (!e)
        text[0] = '\0';
    else if (e->dir)
        snprintf(text, sizeof text, "%s, %s", e->name, e->desc);
    else
        snprintf(text, sizeof text, "%s, %s, %s", e->name, folderview_format_size(e->size, size, sizeof size), e->desc);
    widget_set_text(sel_label, text);
}

static void fv_open(struct folderview *v, const char *path, void *arg)
{
    log_line("open %s", path, NULL);
    pid_t pid = mime_open(path);
    if (pid < 0)
        fail(_("Cannot open the file"), (int)pid);
    else
        folderview_recent_add(path);
}

/* A new folder enters the history unless Back or Forward showed it. */
static void fv_changed(struct folderview *v, void *arg)
{
    enum folderview_kind kind = folderview_kind(v);
    if (kind == FOLDERVIEW_FOLDER && strcmp(history[hist_pos], cwd()) != 0) {
        if (hist_len == HISTORY) {
            memmove(history[0], history[1], (size_t)(HISTORY - 1) * sizeof history[0]);
            hist_len--;
            hist_pos--;
        }
        hist_len = hist_pos + 1;
        strlcpy(history[hist_len++], cwd(), sizeof history[0]);
        hist_pos = hist_len - 1;
        log_line("cd %s", cwd(), NULL);
    }
    char title[300];
    const char *name = kind == FOLDERVIEW_RECENT ? _("Recent")
                       : kind == FOLDERVIEW_SEARCH ? _("Search")
                       : strcmp(cwd(), folderview_home_path(v)) == 0 ? _("Home") : fs_basename(cwd());
    snprintf(title, sizeof title, _("%s - Files"), name);
    gui_set_title(window_state_of(win)->win, title);
    widget_set_enabled(back_button, hist_pos > 0);
    widget_set_enabled(forward_button, hist_pos < hist_len - 1);
    int n = folderview_count(v), hidden = folderview_hidden_count(v);
    char items[32], text[64];
    snprintf(items, sizeof items, ngettext("%d item", "%d items", n), n);
    if (hidden)
        snprintf(text, sizeof text, ngettext("%s, %d hidden", "%s, %d hidden", hidden), items, hidden);
    else
        strlcpy(text, items, sizeof text);
    widget_set_text(status, text);
    update_selection_label();
}

static void fv_selected(struct folderview *v, void *arg)
{
    update_selection_label();
}

static void fv_dropped(struct folderview *v, const char *dir, int count, int action, void *arg)
{
    char n[16];
    snprintf(n, sizeof n, "%d", count);
    log_line(action == GUI_DND_MOVE ? "drop moved %s into %s" : "drop copied %s into %s", n, dir);
}

static const struct folderview_ops files_ops = { fv_open, fv_changed, fv_selected, NULL, NULL };

/* ---- handlers: navigation ---- */

static int on_open(struct widget *w, void *args, void *arg)
{
    const struct folderview_entry *e = selected_entry();
    if (!e)
        return 1;
    char path[512];
    strlcpy(path, e->path, sizeof path);
    if (e->dir)
        folderview_navigate(fv, path);
    else
        fv_open(fv, path, NULL);
    return 1;
}

static int on_up(struct widget *w, void *args, void *arg) { folderview_up(fv); return 1; }
static int on_home(struct widget *w, void *args, void *arg) { folderview_home(fv); return 1; }
static int on_root(struct widget *w, void *args, void *arg) { folderview_navigate(fv, "/"); return 1; }
static int on_recent(struct widget *w, void *args, void *arg) { folderview_show_recent(fv); return 1; }
static int on_location(struct widget *w, void *args, void *arg) { folderview_location(fv, NULL); return 1; }
static int on_search(struct widget *w, void *args, void *arg) { folderview_search(fv, ""); return 1; }
static int on_visit(struct widget *w, void *args, void *arg) { folderview_visit(fv); return 1; }
static int on_refresh(struct widget *w, void *args, void *arg) { folderview_reload(fv); return 1; }

static int on_back(struct widget *w, void *args, void *arg)
{
    if (hist_pos > 0) {
        hist_pos--;
        folderview_navigate(fv, history[hist_pos]);
    }
    return 1;
}

static int on_forward(struct widget *w, void *args, void *arg)
{
    if (hist_pos < hist_len - 1) {
        hist_pos++;
        folderview_navigate(fv, history[hist_pos]);
    }
    return 1;
}

/* The check item follows the state of the view, also when Ctrl+H
 * toggles it without the menu. */
static int on_toggle_hidden(struct widget *w, void *args, void *arg)
{
    folderview_set_hidden(fv, !folderview_hidden(fv));
    menuitem_set_check(w, folderview_hidden(fv));
    return 1;
}

static int on_sort(struct widget *w, void *args, void *arg)
{
    folderview_sort(fv, (int)(long)arg, 0);
    menuitem_set_radio(w, 1);
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
    fs_join(path, sizeof path, cwd(), name);
    if (mkdir(path, 0755) < 0) {
        fail(_("Cannot create the folder"), errno);
        return 1;
    }
    log_line("mkdir %s", path, NULL);
    folderview_reload(fv);
    folderview_select_name(fv, name);
    return 1;
}

static int on_new_file(struct widget *w, void *args, void *arg)
{
    char name[NAME_MAX + 1];
    strlcpy(name, _("New file.txt"), sizeof name);
    if (!app_prompt(app, _("New file"), _("Name:"), name, sizeof name) || !name[0])
        return 1;
    char path[512];
    fs_join(path, sizeof path, cwd(), name);
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0644);
    if (fd < 0) {
        fail(_("Cannot create the file"), errno);
        return 1;
    }
    close(fd);
    log_line("create %s", path, NULL);
    folderview_reload(fv);
    folderview_select_name(fv, name);
    return 1;
}

static int on_open_with(struct widget *w, void *args, void *arg)
{
    const struct folderview_entry *e = selected_entry();
    if (!e)
        return 1;
    char command[MIME_COMMAND], path[512];
    strlcpy(path, e->path, sizeof path);
    if (!app_choose_program(app, path, command, sizeof command))
        return 1;
    log_line("open %s with %s", path, command);
    int r = mime_run(command, path);
    if (r < 0)
        fail(_("Cannot start the program"), r);
    else
        folderview_recent_add(path);
    return 1;
}

static int on_terminal(struct widget *w, void *args, void *arg)
{
    const struct folderview_entry *e = selected_entry();
    char path[512];
    strlcpy(path, e && e->dir ? e->path : cwd(), sizeof path);
    log_line("terminal in %s", path, NULL);
    int r = mime_spawn((char *const[]){ "/bin/term", "-d", path, NULL });
    if (r < 0)
        fail(_("Cannot start the terminal"), r);
    return 1;
}

static int on_rename(struct widget *w, void *args, void *arg)
{
    const struct folderview_entry *e = selected_entry();
    if (!e)
        return 1;
    char from[512], dir[512], name[NAME_MAX + 1], to[512];
    strlcpy(from, e->path, sizeof from);
    strlcpy(name, e->name, sizeof name);
    if (!app_prompt(app, _("Rename"), _("New name:"), name, sizeof name) || !name[0] ||
        strcmp(name, fs_basename(from)) == 0)
        return 1;
    strlcpy(dir, from, sizeof dir);
    fs_join(to, sizeof to, dir, "..");
    fs_normalize(to);
    strlcpy(dir, to, sizeof dir);
    fs_join(to, sizeof to, dir, name);
    if (rename(from, to) < 0) {
        fail(_("Cannot rename"), errno);
        return 1;
    }
    log_line("rename %s -> %s", from, to);
    folderview_reload(fv);
    folderview_select_name(fv, name);
    return 1;
}

static int on_delete(struct widget *w, void *args, void *arg)
{
    const struct folderview_entry *e = selected_entry();
    if (!e)
        return 1;
    char text[300], path[512];
    strlcpy(path, e->path, sizeof path);
    if (e->dir)
        snprintf(text, sizeof text, _("Delete \"%s\" and everything in it?"), e->name);
    else
        snprintf(text, sizeof text, _("Delete \"%s\"?"), e->name);
    const char *const buttons[] = { _("Delete"), _("Cancel") };
    if (app_dialog(app, _("Delete"), text, buttons, 2) != 0)
        return 1;
    int r = fileops_remove(path);
    if (r < 0) {
        fail(_("Cannot delete"), r);
        return 1;
    }
    log_line("delete %s", path, NULL);
    folderview_reload(fv);
    return 1;
}

static int on_copy(struct widget *w, void *args, void *arg)
{
    const struct folderview_entry *e = selected_entry();
    if (!e)
        return 1;
    strlcpy(clip_path, e->path, sizeof clip_path);
    clip_cut = (int)(long)arg;
    gui_clipboard_set(clip_path, (int)strlen(clip_path));
    return 1;
}

static int on_paste(struct widget *w, void *args, void *arg)
{
    if (!clip_path[0])
        return 1;
    char to[512];
    fs_join(to, sizeof to, cwd(), fs_basename(clip_path));
    if (strcmp(to, clip_path) == 0) {
        if (clip_cut)
            return 1;
        char name[NAME_MAX + 1];
        snprintf(name, sizeof name, _("Copy of %s"), fs_basename(clip_path));
        fs_join(to, sizeof to, cwd(), name);
    }
    struct stat st;
    if (stat(to, &st) == 0) {
        fail(_("Cannot paste"), EEXIST);
        return 1;
    }
    int r = clip_cut ? fileops_move(clip_path, to) : fileops_copy(clip_path, to);
    if (r < 0) {
        fail(clip_cut ? _("Cannot move") : _("Cannot copy"), r);
        return 1;
    }
    log_line(clip_cut ? "move %s -> %s" : "copy %s -> %s", clip_path, to);
    if (clip_cut) {
        strlcpy(clip_path, to, sizeof clip_path);
        clip_cut = 0;
    }
    folderview_reload(fv);
    folderview_select_name(fv, fs_basename(to));
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
    const struct folderview_entry *e = selected_entry();
    char path[512], location[512], size[64], date[64], count[96], files_text[48], dirs_text[48];
    strlcpy(path, e ? e->path : cwd(), sizeof path);
    fs_join(location, sizeof location, path, "..");
    fs_normalize(location);
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
    prop_row(grid, 1, _("Location"), location);
    prop_row(grid, 2, _("Type"), folderview_describe(type, (st.st_mode & 0111) != 0));
    if (S_ISDIR(st.st_mode)) {
        snprintf(files_text, sizeof files_text, ngettext("%d file", "%d files", files), files);
        snprintf(dirs_text, sizeof dirs_text, ngettext("%d folder", "%d folders", dirs - 1), dirs - 1);
        snprintf(count, sizeof count, "%s, %s", files_text, dirs_text);
        prop_row(grid, 3, _("Contents"), count);
        prop_row(grid, 4, _("Size"), folderview_format_size(bytes, size, sizeof size));
    } else {
        snprintf(count, sizeof count, ngettext("%s (%ld byte)", "%s (%ld bytes)", bytes),
                 folderview_format_size(bytes, size, sizeof size), bytes);
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

/* The entry menu on an entry and the folder menu elsewhere.  Open item
 * location is shown only in Recent and in searches. */
static int on_context(struct widget *w, void *args, void *arg)
{
    struct sig_click *c = args;
    int x, y;
    widget_abs(w, &x, &y);
    widget_set_visible(location_item, folderview_kind(fv) != FOLDERVIEW_FOLDER);
    menu_popup(selected_entry() ? item_menu : dir_menu, x + c->x, y + c->y);
    return 1;
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
    menu_add_separator(edit);
    item(edit, _("Search"), "search", on_search, NULL, KEY_F, WMOD_CTRL);
    struct widget *view = menu_new(bar, _("View"));
    item(view, _("Refresh"), "refresh", on_refresh, NULL, KEY_F5, 0);
    /* The view shows no hidden files at the start. */
    menuitem_set_check(item(view, _("Show hidden files"), NULL, on_toggle_hidden, NULL, KEY_H, WMOD_CTRL), 0);
    menu_add_separator(view);
    static const char *const sorts[] = { N_("Sort by name"), N_("Sort by size"), N_("Sort by type"), N_("Sort by date") };
    for (long i = 0; i < 4; i++)
        menuitem_set_radio(item(view, _(sorts[i]), NULL, on_sort, (void *)i, 0, 0), i == 0);
    struct widget *go = menu_new(bar, _("Go"));
    item(go, _("Back"), "back", on_back, NULL, KEY_LEFT, WMOD_ALT);
    item(go, _("Forward"), "forward", on_forward, NULL, KEY_RIGHT, WMOD_ALT);
    item(go, _("Parent folder"), "up", on_up, NULL, KEY_UP, WMOD_ALT);
    menu_add_separator(go);
    item(go, _("Home"), "home", on_home, NULL, KEY_HOME, WMOD_ALT);
    item(go, _("Recent"), "recent", on_recent, NULL, 0, 0);
    item(go, _("Root"), "drive", on_root, NULL, 0, 0);
    item(go, _("Location..."), NULL, on_location, NULL, KEY_L, WMOD_CTRL);

    item_menu = popupmenu_new(win);
    item(item_menu, _("Open"), "open", on_open, NULL, 0, 0);
    item(item_menu, _("Open with..."), NULL, on_open_with, NULL, 0, 0);
    location_item = item(item_menu, _("Open item location"), "folder", on_visit, NULL, 0, 0);
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

int main(int argc, char **argv)
{
    app = app_create();
    if (!app)
        return 1;
    textdomain("files");
    /* The window is mapped with the title of the folder it opens, which
     * fv_changed sets again on every later change. */
    char title[300];
    snprintf(title, sizeof title, _("%s - Files"), argc > 1 ? fs_basename(argv[1]) : _("Home"));
    win = app_window(app, 760, 480, title);
    if (!win)
        return 1;
    build_menus();
    /* The tool bar contains Back and Forward, followed by the path bar, the
     * location entry, the search field and the search button of the
     * folder view. */
    struct widget *bar = toolbar_new(win);
    back_button = toolbar_add(bar, "back", _("Back"));
    widget_connect(back_button, "clicked", on_back, NULL);
    forward_button = toolbar_add(bar, "forward", _("Forward"));
    widget_connect(forward_button, "clicked", on_forward, NULL);
    struct widget *split = splitpane_new(win, 0);
    struct widget *sb = statusbar_new(win);
    status = statusbar_add(sb, 1);
    sel_label = statusbar_add(sb, 0);
    fv = folderview_new(bar, split, _("Files"), &files_ops, NULL);
    if (!fv)
        return 1;
    folderview_on_dropped(fv, fv_dropped);
    widget_connect(folderview_table(fv), "context", on_context, NULL);
    strlcpy(history[0], cwd(), sizeof history[0]);
    hist_len = 1;
    hist_pos = 0;
    if (argc > 1)
        folderview_navigate(fv, argv[1]);
    else
        fv_changed(fv, NULL);
    widget_focus(folderview_table(fv));
    app_run(app);
    app_destroy(app);
    return 0;
}
