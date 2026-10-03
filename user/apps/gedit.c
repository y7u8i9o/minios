/* gedit: a graphical text editor on the editor widget.
 *
 * The window has File and Edit menus with accelerators, a tool bar, the
 * editor and a status bar with a message, the language, the cursor
 * position and the number of lines.  The Edit menu and the context menu
 * of the editor contain Undo, Redo, Cut, Copy, Paste, Delete and Select
 * all.  Find, Find next, Replace and Go to line ask for their text in a
 * dialog.  Highlighting follows the extension of the file.  New, Open,
 * Quit and closing the window ask before unsaved changes are lost. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <gui/app.h>
#include <gui/i18n.h>

static struct app *app;
static struct widget *win, *editor, *status, *lang_label, *pos_label, *lines_label;
static struct widget *undo_item, *redo_item, *cut_item, *copy_item, *delete_item;
static struct widget *undo_button, *redo_button, *cut_button, *copy_button;
static char path[256];
static char needle[128], replacement[128];

static const char *base_name(void)
{
    const char *slash = strrchr(path, '/');
    return path[0] ? (slash ? slash + 1 : path) : _("untitled");
}

static void update_title(void)
{
    char title[300];
    snprintf(title, sizeof title, "%s%s - gedit", base_name(), editor_modified(editor) ? " *" : "");
    gui_set_title(window_state_of(win)->win, title);
}

/* update_commands enables the commands that apply to the current state of
 * the editor. */
static void update_commands(void)
{
    int sel = editor_has_selection(editor);
    widget_set_enabled(undo_item, editor_can_undo(editor));
    widget_set_enabled(undo_button, editor_can_undo(editor));
    widget_set_enabled(redo_item, editor_can_redo(editor));
    widget_set_enabled(redo_button, editor_can_redo(editor));
    widget_set_enabled(cut_item, sel);
    widget_set_enabled(cut_button, sel);
    widget_set_enabled(copy_item, sel);
    widget_set_enabled(copy_button, sel);
    widget_set_enabled(delete_item, sel);
}

static void set_highlighter(void)
{
    const char *dot = strrchr(path, '.');
    const char *lang = _("Plain text");
    if (dot && (strcmp(dot, ".c") == 0 || strcmp(dot, ".h") == 0)) {
        editor_set_highlighter(editor, highlight_c, NULL);
        lang = "C";
    } else if (dot && strcmp(dot, ".sh") == 0) {
        editor_set_highlighter(editor, highlight_sh, NULL);
        lang = "Shell";
    } else if (dot && strcmp(dot, ".lua") == 0) {
        editor_set_highlighter(editor, highlight_lang, (void *)&highlight_language_lua);
        lang = "Lua";
    } else {
        editor_set_highlighter(editor, NULL, NULL);
    }
    widget_set_text(lang_label, lang);
}

static void show_message(const char *text)
{
    widget_set_text(status, text);
}

static int on_cursor(struct widget *w, void *args, void *arg)
{
    int l, c;
    editor_cursor(editor, &l, &c);
    char s[48];
    snprintf(s, sizeof s, _("Ln %d, Col %d"), l + 1, c + 1);
    widget_set_text(pos_label, s);
    int lines = editor_line_count(editor);
    snprintf(s, sizeof s, ngettext("%d line", "%d lines", (unsigned long)lines), lines);
    widget_set_text(lines_label, s);
    update_commands();
    return 0;
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
    on_cursor(NULL, NULL, NULL);
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
    char msg[300];
    snprintf(msg, sizeof msg, _("Saved %s"), base_name());
    show_message(msg);
    printf("gedit: saved %s\n", path);
    fflush(stdout);
    return 0;
}

static void error(const char *text)
{
    const char *const buttons[] = { _("OK") };
    app_dialog(app, _("Error"), text, buttons, 1);
}

static const struct file_filter text_filters[] = {
    { N_("All files"), "*" }, { N_("Text files"), "text/*" },
};

/* The filters with their names translated. */
static int filters(struct file_filter *out)
{
    int n = (int)(sizeof text_filters / sizeof text_filters[0]);
    for (int i = 0; i < n; i++)
        out[i] = (struct file_filter){ _(text_filters[i].name), text_filters[i].patterns };
    return n;
}

static int on_save_as(struct widget *w, void *args, void *arg)
{
    char name[256];
    struct file_filter f[2];
    int nf = filters(f);
    strlcpy(name, path, sizeof name);
    if (app_choose_file(app, FILE_CHOOSER_SAVE, _("Save as"), f, nf, name, sizeof name)) {
        strlcpy(path, name, sizeof path);
        set_highlighter();
        if (save() < 0)
            error(_("The file cannot be written."));
    }
    return 1;
}

static int on_save(struct widget *w, void *args, void *arg)
{
    if (!path[0])
        return on_save_as(w, args, arg);
    if (save() < 0)
        error(_("The file cannot be written."));
    return 1;
}

/* may_discard asks what to do with unsaved changes.  It returns 1 when
 * the text may be replaced or the window closed. */
static int may_discard(void)
{
    if (!editor_modified(editor))
        return 1;
    const char *const buttons[] = { _("Save"), _("Discard"), _("Cancel") };
    int r = app_dialog(app, _("Unsaved changes"), _("Save the changes?"), buttons, 3);
    if (r == 0) {
        on_save(NULL, NULL, NULL);
        return !editor_modified(editor);
    }
    return r == 1;
}

static int on_new(struct widget *w, void *args, void *arg)
{
    if (!may_discard())
        return 1;
    editor_set_text(editor, "");
    path[0] = '\0';
    set_highlighter();
    update_title();
    on_cursor(NULL, NULL, NULL);
    return 1;
}

static int on_open(struct widget *w, void *args, void *arg)
{
    if (!may_discard())
        return 1;
    char name[256];
    struct file_filter f[2];
    int nf = filters(f);
    strlcpy(name, path, sizeof name);
    if (app_choose_file(app, FILE_CHOOSER_OPEN, _("Open"), f, nf, name, sizeof name) && load(name) < 0)
        error(_("The file cannot be opened."));
    return 1;
}

static int on_quit(struct widget *w, void *args, void *arg)
{
    if (may_discard())
        app_quit(app, 0);
    return 1;
}

static int on_close(struct widget *w, void *args, void *arg)
{
    return !may_discard();
}

static int on_undo(struct widget *w, void *args, void *arg) { editor_undo(editor); on_cursor(NULL, NULL, NULL); return 1; }
static int on_redo(struct widget *w, void *args, void *arg) { editor_redo(editor); on_cursor(NULL, NULL, NULL); return 1; }
static int on_cut(struct widget *w, void *args, void *arg) { editor_cut(editor); widget_focus(editor); return 1; }
static int on_copy(struct widget *w, void *args, void *arg) { editor_copy(editor); widget_focus(editor); return 1; }
static int on_paste(struct widget *w, void *args, void *arg) { editor_paste(editor); widget_focus(editor); return 1; }
static int on_delete(struct widget *w, void *args, void *arg) { editor_delete_selection(editor); widget_focus(editor); return 1; }
static int on_select_all(struct widget *w, void *args, void *arg) { editor_select_all(editor); widget_focus(editor); return 1; }

/* selection_needle copies a selection within one line into buf. */
static void selection_needle(char *buf, size_t size)
{
    char *sel = editor_selection(editor);
    if (sel && !strchr(sel, '\n'))
        strlcpy(buf, sel, size);
    free(sel);
}

static void find_next(void)
{
    if (!needle[0])
        return;
    if (!editor_find(editor, needle, 1)) {
        char msg[200];
        snprintf(msg, sizeof msg, _("\"%s\" not found"), needle);
        show_message(msg);
    } else {
        show_message("");
    }
}

static int on_find(struct widget *w, void *args, void *arg)
{
    selection_needle(needle, sizeof needle);
    if (app_prompt(app, _("Find"), _("Text:"), needle, sizeof needle))
        find_next();
    widget_focus(editor);
    return 1;
}

static int on_find_next(struct widget *w, void *args, void *arg)
{
    find_next();
    return 1;
}

static int on_replace(struct widget *w, void *args, void *arg)
{
    selection_needle(needle, sizeof needle);
    if (app_prompt(app, _("Replace"), _("Find:"), needle, sizeof needle) && needle[0] &&
        app_prompt(app, _("Replace"), _("Replace with:"), replacement, sizeof replacement)) {
        int n = editor_replace_all(editor, needle, replacement);
        char msg[128];
        snprintf(msg, sizeof msg, ngettext("%d occurrence replaced", "%d occurrences replaced", (unsigned long)n), n);
        show_message(msg);
        printf("gedit: %d replaced\n", n);
        fflush(stdout);
    }
    widget_focus(editor);
    return 1;
}

static int on_goto(struct widget *w, void *args, void *arg)
{
    char line[16] = "";
    if (app_prompt(app, _("Go to line"), _("Line:"), line, sizeof line)) {
        int n = atoi(line);
        if (n > 0)
            editor_goto(editor, n - 1, 0);
    }
    widget_focus(editor);
    return 1;
}

static int on_wrap(struct widget *w, void *args, void *arg) { editor_set_wrap(editor, w->value); return 1; }
static int on_numbers(struct widget *w, void *args, void *arg) { editor_set_line_numbers(editor, w->value); return 1; }

static int on_changed(struct widget *w, void *args, void *arg)
{
    update_title();
    show_message("");
    return on_cursor(w, args, arg);
}

static struct widget *add_item(struct widget *menu, const char *text, const char *icon, signal_fn fn, int key, int mods)
{
    struct widget *m = menu_add(menu, text, icon);
    widget_connect(m, "clicked", fn, NULL);
    if (key)
        widget_set_accel(m, key, mods);
    return m;
}

int main(int argc, char **argv)
{
    app = app_create();
    if (!app)
        return 1;
    textdomain("gedit");
    win = app_window(app, 680, 480, "gedit");
    if (!win)
        return 1;
    struct widget *bar = menubar_new(win);
    struct widget *file = menu_new(bar, _("File"));
    add_item(file, _("New"), "new", on_new, KEY_N, WMOD_CTRL);
    add_item(file, _("Open..."), "open", on_open, KEY_O, WMOD_CTRL);
    add_item(file, _("Save"), "save", on_save, KEY_S, WMOD_CTRL);
    add_item(file, _("Save as..."), NULL, on_save_as, KEY_S, WMOD_CTRL | WMOD_SHIFT);
    menu_add_separator(file);
    add_item(file, _("Quit"), "quit", on_quit, KEY_Q, WMOD_CTRL);
    struct widget *edit = menu_new(bar, _("Edit"));
    undo_item = add_item(edit, _("Undo"), "undo", on_undo, KEY_Z, WMOD_CTRL);
    redo_item = add_item(edit, _("Redo"), "redo", on_redo, KEY_Y, WMOD_CTRL);
    menu_add_separator(edit);
    cut_item = add_item(edit, _("Cut"), "cut", on_cut, KEY_X, WMOD_CTRL);
    copy_item = add_item(edit, _("Copy"), "copy", on_copy, KEY_C, WMOD_CTRL);
    add_item(edit, _("Paste"), "paste", on_paste, KEY_V, WMOD_CTRL);
    delete_item = add_item(edit, _("Delete"), NULL, on_delete, 0, 0);
    add_item(edit, _("Select all"), NULL, on_select_all, KEY_A, WMOD_CTRL);
    menu_add_separator(edit);
    add_item(edit, _("Find..."), "search", on_find, KEY_F, WMOD_CTRL);
    add_item(edit, _("Find next"), NULL, on_find_next, KEY_F3, 0);
    add_item(edit, _("Replace..."), NULL, on_replace, KEY_H, WMOD_CTRL);
    add_item(edit, _("Go to line..."), NULL, on_goto, KEY_L, WMOD_CTRL);

    struct widget *tools = toolbar_new(win);
    widget_connect(toolbar_add(tools, "new", _("New")), "clicked", on_new, NULL);
    widget_connect(toolbar_add(tools, "open", _("Open")), "clicked", on_open, NULL);
    widget_connect(toolbar_add(tools, "save", _("Save")), "clicked", on_save, NULL);
    separator_new(tools);
    undo_button = toolbar_add(tools, "undo", _("Undo"));
    widget_connect(undo_button, "clicked", on_undo, NULL);
    redo_button = toolbar_add(tools, "redo", _("Redo"));
    widget_connect(redo_button, "clicked", on_redo, NULL);
    separator_new(tools);
    cut_button = toolbar_add(tools, "cut", _("Cut"));
    widget_connect(cut_button, "clicked", on_cut, NULL);
    copy_button = toolbar_add(tools, "copy", _("Copy"));
    widget_connect(copy_button, "clicked", on_copy, NULL);
    widget_connect(toolbar_add(tools, "paste", _("Paste")), "clicked", on_paste, NULL);
    separator_new(tools);
    widget_connect(toolbar_add(tools, "search", _("Find")), "clicked", on_find, NULL);
    struct widget *wrap = checkbox_new(tools, _("Wrap"));
    widget_connect(wrap, "toggled", on_wrap, NULL);
    struct widget *nums = checkbox_new(tools, _("Line numbers"));
    widget_connect(nums, "toggled", on_numbers, NULL);

    editor = editor_new(win);
    widget_connect(editor, "changed", on_changed, NULL);
    widget_connect(editor, "cursor", on_cursor, NULL);
    widget_connect(win, "close", on_close, NULL);
    struct widget *sb = statusbar_new(win);
    status = statusbar_add(sb, 1);
    lang_label = statusbar_add(sb, 0);
    widget_set_min(lang_label, 80, 0);
    pos_label = statusbar_add(sb, 0);
    widget_set_min(pos_label, 110, 0);
    lines_label = statusbar_add(sb, 0);
    widget_set_min(lines_label, 80, 0);
    set_highlighter();
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
