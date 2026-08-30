/* view: a read only editor showing a file, with menus, a tool bar,
 * highlighting by extension and a status bar. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <gui/app.h>

static struct app *app;
static struct widget *win, *editor, *status;
static char path[256];

static char *read_file(const char *name)
{
    FILE *f = fopen(name, "r");
    if (!f)
        return NULL;
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
    if (!text)
        return strdup("");
    text[len] = '\0';
    return text;
}

static void apply_highlighter(struct widget *ed, const char *name)
{
    const char *dot = strrchr(name, '.');
    if (dot && (strcmp(dot, ".c") == 0 || strcmp(dot, ".h") == 0))
        editor_set_highlighter(ed, highlight_c, NULL);
    else if (dot && strcmp(dot, ".sh") == 0)
        editor_set_highlighter(ed, highlight_sh, NULL);
    else
        editor_set_highlighter(ed, NULL, NULL);
}

static int load(const char *name)
{
    char *text = read_file(name);
    if (!text)
        return -1;
    editor_set_text(editor, text);
    free(text);
    apply_highlighter(editor, name);
    strlcpy(path, name, sizeof path);
    char title[300];
    snprintf(title, sizeof title, "view: %s", name);
    widget_set_text(win, title);
    gui_set_title(window_state_of(win)->win, title);
    snprintf(title, sizeof title, "%d lines", editor_line_count(editor));
    widget_set_text(status, title);
    return 0;
}

static int on_open(struct widget *w, void *args, void *arg)
{
    char name[256];
    strlcpy(name, path, sizeof name);
    if (app_prompt(app, "Open", "File:", name, sizeof name) && load(name) < 0) {
        const char *const buttons[] = { "OK" };
        app_dialog(app, "Error", "The file cannot be opened.", buttons, 1);
    }
    return 1;
}

static int on_quit(struct widget *w, void *args, void *arg) { app_quit(app, 0); return 1; }
static int on_wrap(struct widget *w, void *args, void *arg) { editor_set_wrap(editor, w->value); return 1; }
static int on_numbers(struct widget *w, void *args, void *arg) { editor_set_line_numbers(editor, w->value); return 1; }

static int on_scale(struct widget *w, void *args, void *arg)
{
    app_theme(app)->scale = (int)(long)arg;
    app_theme_changed(app);
    return 1;
}

int main(int argc, char **argv)
{
    app = app_create();
    if (!app)
        return 1;
    win = app_window(app, 520, 360, "view");
    if (!win)
        return 1;
    struct widget *bar = menubar_new(win);
    struct widget *file = menu_new(bar, "File");
    widget_connect(menu_add(file, "Open...", "open"), "clicked", on_open, NULL);
    menu_add_separator(file);
    widget_connect(menu_add(file, "Quit", "quit"), "clicked", on_quit, NULL);
    struct widget *vw = menu_new(bar, "View");
    widget_connect(menu_add(vw, "Normal size", NULL), "clicked", on_scale, (void *)100);
    widget_connect(menu_add(vw, "Large size", NULL), "clicked", on_scale, (void *)125);
    struct widget *tools = toolbar_new(win);
    widget_connect(toolbar_add(tools, "open", "Open"), "clicked", on_open, NULL);
    widget_connect(toolbar_add(tools, "quit", "Quit"), "clicked", on_quit, NULL);
    struct widget *wrap = checkbox_new(tools, "Wrap");
    widget_connect(wrap, "toggled", on_wrap, NULL);
    struct widget *nums = checkbox_new(tools, "Numbers");
    widget_connect(nums, "toggled", on_numbers, NULL);
    editor = editor_new(win);
    editor_set_readonly(editor, 1);
    struct widget *sb = statusbar_new(win);
    status = statusbar_add(sb, 1);
    if (argc > 1 && load(argv[1]) < 0) {
        fprintf(stderr, "view: cannot open %s\n", argv[1]);
        return 1;
    }
    widget_focus(editor);
    app_run(app);
    app_destroy(app);
    return 0;
}
