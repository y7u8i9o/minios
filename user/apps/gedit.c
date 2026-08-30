/* gedit: a graphical text editor on the editor widget: menus, a tool
 * bar, undo and redo, search, word wrap, line numbers, highlighting by
 * extension, and a status bar with the cursor position. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <gui/app.h>

static struct app *app;
static struct widget *win, *editor, *status, *pos_label;
static char path[256];
static char needle[128];

static void update_title(void)
{
    char title[300];
    snprintf(title, sizeof title, "%s%s - gedit", path[0] ? path : "untitled", editor_modified(editor) ? " *" : "");
    gui_set_title(window_state_of(win)->win, title);
}

static void set_highlighter(void)
{
    const char *dot = strrchr(path, '.');
    if (dot && (strcmp(dot, ".c") == 0 || strcmp(dot, ".h") == 0))
        editor_set_highlighter(editor, highlight_c, NULL);
    else if (dot && strcmp(dot, ".sh") == 0)
        editor_set_highlighter(editor, highlight_sh, NULL);
    else
        editor_set_highlighter(editor, NULL, NULL);
}

static int load(const char *name)
{
    FILE *f = fopen(name, "r");
    if (!f)
        return -1;
    char *text = NULL;
    size_t len = 0, cap = 0, n;
    char buf[512];
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) {
        if (len + n + 1 > cap) {
            cap = (len + n + 1) * 2;
            text = realloc(text, cap);
        }
        memcpy(text + len, buf, n);
        len += n;
    }
    fclose(f);
    if (text) {
        text[len] = '\0';
        editor_set_text(editor, text);
        free(text);
    } else {
        editor_set_text(editor, "");
    }
    strlcpy(path, name, sizeof path);
    set_highlighter();
    update_title();
    return 0;
}

static int save(void)
{
    if (!path[0])
        return -1;
    FILE *f = fopen(path, "w");
    if (!f)
        return -1;
    char *text = editor_text(editor);
    fputs(text, f);
    fclose(f);
    free(text);
    editor_set_modified(editor, 0);
    update_title();
    printf("gedit: saved %s\n", path);
    fflush(stdout);
    return 0;
}

static void error(const char *text)
{
    const char *const buttons[] = { "OK" };
    app_dialog(app, "Error", text, buttons, 1);
}

static int on_new(struct widget *w, void *args, void *arg)
{
    editor_set_text(editor, "");
    path[0] = '\0';
    set_highlighter();
    update_title();
    return 1;
}

static int on_open(struct widget *w, void *args, void *arg)
{
    char name[256];
    strlcpy(name, path, sizeof name);
    if (app_prompt(app, "Open", "File:", name, sizeof name) && load(name) < 0)
        error("The file cannot be opened.");
    return 1;
}

static int on_save_as(struct widget *w, void *args, void *arg)
{
    char name[256];
    strlcpy(name, path, sizeof name);
    if (app_prompt(app, "Save as", "File:", name, sizeof name)) {
        strlcpy(path, name, sizeof path);
        set_highlighter();
        if (save() < 0)
            error("The file cannot be written.");
    }
    return 1;
}

static int on_save(struct widget *w, void *args, void *arg)
{
    if (!path[0])
        return on_save_as(w, args, arg);
    if (save() < 0)
        error("The file cannot be written.");
    return 1;
}

static int on_quit(struct widget *w, void *args, void *arg) { app_quit(app, 0); return 1; }
static int on_undo(struct widget *w, void *args, void *arg) { editor_undo(editor); return 1; }
static int on_redo(struct widget *w, void *args, void *arg) { editor_redo(editor); return 1; }

static int on_find(struct widget *w, void *args, void *arg)
{
    if (app_prompt(app, "Find", "Text:", needle, sizeof needle) && !editor_find(editor, needle, 1))
        widget_set_text(status, "not found");
    widget_focus(editor);
    return 1;
}

static int on_find_next(struct widget *w, void *args, void *arg)
{
    if (needle[0] && !editor_find(editor, needle, 1))
        widget_set_text(status, "not found");
    return 1;
}

static int on_wrap(struct widget *w, void *args, void *arg) { editor_set_wrap(editor, w->value); return 1; }
static int on_numbers(struct widget *w, void *args, void *arg) { editor_set_line_numbers(editor, w->value); return 1; }

static int on_cursor(struct widget *w, void *args, void *arg)
{
    int l, c;
    editor_cursor(editor, &l, &c);
    char s[48];
    snprintf(s, sizeof s, "line %d, column %d", l + 1, c + 1);
    widget_set_text(pos_label, s);
    return 0;
}

static int on_changed(struct widget *w, void *args, void *arg)
{
    update_title();
    widget_set_text(status, "");
    return on_cursor(w, args, arg);
}

static int on_close(struct widget *w, void *args, void *arg)
{
    if (!editor_modified(editor))
        return 0;
    const char *const buttons[] = { "Save", "Discard", "Cancel" };
    int r = app_dialog(app, "Unsaved changes", "Save the changes before closing?", buttons, 3);
    if (r == 0)
        on_save(w, args, arg);
    return r == 2 || r < 0;
}

int main(int argc, char **argv)
{
    app = app_create();
    if (!app)
        return 1;
    win = app_window(app, 600, 420, "gedit");
    if (!win)
        return 1;
    struct widget *bar = menubar_new(win);
    struct widget *file = menu_new(bar, "File");
    widget_connect(menu_add(file, "New", "new"), "clicked", on_new, NULL);
    widget_connect(menu_add(file, "Open...", "open"), "clicked", on_open, NULL);
    widget_connect(menu_add(file, "Save", "save"), "clicked", on_save, NULL);
    widget_connect(menu_add(file, "Save as...", NULL), "clicked", on_save_as, NULL);
    menu_add_separator(file);
    widget_connect(menu_add(file, "Quit", "quit"), "clicked", on_quit, NULL);
    struct widget *edit = menu_new(bar, "Edit");
    widget_connect(menu_add(edit, "Undo", NULL), "clicked", on_undo, NULL);
    widget_connect(menu_add(edit, "Redo", NULL), "clicked", on_redo, NULL);
    menu_add_separator(edit);
    widget_connect(menu_add(edit, "Find...", "search"), "clicked", on_find, NULL);
    widget_connect(menu_add(edit, "Find next", NULL), "clicked", on_find_next, NULL);
    struct widget *tools = toolbar_new(win);
    widget_connect(toolbar_add(tools, "new", "New"), "clicked", on_new, NULL);
    widget_connect(toolbar_add(tools, "open", "Open"), "clicked", on_open, NULL);
    struct widget *save_button = toolbar_add(tools, "save", "Save");
    widget_connect(save_button, "clicked", on_save, NULL);
    widget_set_accel(save_button, 0x1f, WMOD_CTRL);           /* Ctrl+S */
    struct widget *find_button = toolbar_add(tools, "search", "Find");
    widget_connect(find_button, "clicked", on_find, NULL);
    widget_set_accel(find_button, 0x21, WMOD_CTRL);           /* Ctrl+F */
    struct widget *wrap = checkbox_new(tools, "Wrap");
    widget_connect(wrap, "toggled", on_wrap, NULL);
    struct widget *nums = checkbox_new(tools, "Numbers");
    widget_connect(nums, "toggled", on_numbers, NULL);
    editor = editor_new(win);
    widget_connect(editor, "changed", on_changed, NULL);
    widget_connect(editor, "cursor", on_cursor, NULL);
    widget_connect(win, "close", on_close, NULL);
    struct widget *sb = statusbar_new(win);
    status = statusbar_add(sb, 1);
    pos_label = statusbar_add(sb, 0);
    if (argc > 1) {
        if (load(argv[1]) < 0) {
            strlcpy(path, argv[1], sizeof path);
            set_highlighter();
        }
    }
    update_title();
    on_cursor(NULL, NULL, NULL);
    widget_focus(editor);
    app_run(app);
    app_destroy(app);
    return 0;
}
