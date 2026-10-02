/* M22: controls, containers, menus, models and the editor. */
#include <gui/app.h>
#include <gui/model.h>
#include <stdlib.h>
#include <string.h>
#include "check.h"

struct app *app_create_detached(void);

/* WM_KEY carries input-core KEY_* values, not PS/2 scancodes. */
static struct wmsg key_msg(struct widget *win, int code, int ch, int mods)
{
    struct wmsg m = { .type = WM_KEY, .window = window_state_of(win)->win->id, .a = code, .b = 1, .c = mods, .d = ch };
    return m;
}

static struct wmsg mouse_msg(struct widget *win, int kind, int x, int y, int buttons)
{
    struct wmsg m = { .type = WM_MOUSE, .window = window_state_of(win)->win->id, .a = x, .b = y, .c = buttons, .d = kind };
    return m;
}

static void click(struct widget *win, int x, int y)
{
    struct wmsg d = mouse_msg(win, WMOUSE_DOWN, x, y, 1), u = mouse_msg(win, WMOUSE_UP, x, y, 0);
    window_message(win, &d);
    window_message(win, &u);
}

static void type_text(struct widget *win, const char *s)
{
    for (; *s; s++) {
        struct wmsg m = key_msg(win, *s == '\n' ? KEY_ENTER : 0, *s, 0);
        window_message(win, &m);
    }
}

static int last_value = -1;
static int on_change(struct widget *w, void *args, void *arg) { last_value = ((struct sig_change *)args)->value; return 0; }
static int on_select(struct widget *w, void *args, void *arg) { last_value = ((struct sig_select *)args)->index; return 0; }
static int clicks;
static int on_click(struct widget *w, void *args, void *arg) { clicks++; return 1; }

static void test_controls(struct app *a)
{
    struct widget *win = app_window(a, 300, 300, "controls");
    struct widget *combo = combobox_new(win);
    combobox_add(combo, "red");
    combobox_add(combo, "green");
    combobox_add(combo, "blue");
    widget_connect(combo, "changed", on_select, NULL);
    struct widget *spin = spinner_new(win, 0, 10, 5);
    widget_connect(spin, "changed", on_change, NULL);
    struct widget *slider = slider_new(win, 0, 100, 0);
    widget_connect(slider, "changed", on_change, NULL);
    struct widget *prog = progress_new(win);
    widget_set_value(prog, 40);
    window_paint(win);
    CHECK(combo->value == 0 && strcmp(widget_text(combo), "red") == 0, "combo box starts at the first item");
    /* Open the popup, pick the third item. */
    int ax, ay;
    widget_abs(combo, &ax, &ay);
    click(win, ax + 5, ay + 5);
    struct window_state *ws = window_state_of(win);
    CHECK(ws->popup != NULL, "combo popup opened");
    if (ws->popup) {
        window_paint(win);
        int lh = app_theme(a)->font->height + 4;
        click(win, ws->popup->x + 10, ws->popup->y + 1 + 2 * lh + 2);
        CHECK(ws->popup == NULL, "popup closed after the pick");
        CHECK(combo->value == 2 && last_value == 2, "third item selected: %d", combo->value);
    }
    /* Escape closes a popup, an outside click too. */
    click(win, ax + 5, ay + 5);
    struct wmsg esc = key_msg(win, KEY_ESC, 27, 0);
    window_message(win, &esc);
    CHECK(ws->popup == NULL, "Escape closes the popup");
    click(win, ax + 5, ay + 5);
    click(win, 290, 290);
    CHECK(ws->popup == NULL, "outside click closes the popup");
    /* Spinner keys and arrows. */
    widget_focus(spin);
    struct wmsg up = key_msg(win, KEY_UP, 0, 0);
    window_message(win, &up);
    CHECK(spin->value == 6 && last_value == 6, "spinner up: %d", spin->value);
    widget_abs(spin, &ax, &ay);
    click(win, ax + spin->w - 5, ay + spin->h - 3);
    CHECK(spin->value == 5, "spinner down arrow: %d", spin->value);
    /* Slider: click at three quarters. */
    widget_abs(slider, &ax, &ay);
    click(win, ax + (slider->w - 10) * 3 / 4 + 5, ay + slider->h / 2);
    CHECK(slider->value >= 73 && slider->value <= 77, "slider value %d", slider->value);
    window_close(win);
}

static void test_containers(struct app *a)
{
    struct widget *win = app_window(a, 300, 300, "containers");
    struct widget *tabs = tabs_new(win);
    struct widget *p1 = tabs_add(tabs, "One"), *p2 = tabs_add(tabs, "Two");
    struct widget *b1 = button_new(p1, "in one"), *b2 = button_new(p2, "in two");
    widget_connect(tabs, "changed", on_select, NULL);
    window_paint(win);
    CHECK(p1->visible && !p2->visible, "first page visible");
    struct painter pt;
    struct surface dummy = { NULL, 0, 0, 0 };
    painter_init(&pt, &dummy, app_theme(a));
    int w1 = painter_text_width(&pt, "One", -1) + 20;
    int ax, ay;
    widget_abs(tabs, &ax, &ay);
    click(win, ax + w1 + 5, ay + 5);
    window_paint(win);
    CHECK(tabs->value == 1 && last_value == 1 && p2->visible && !p1->visible, "second tab selected");
    CHECK(b2->w > 0 && b1->parent == p1, "pages hold their children");
    window_close(win);

    win = app_window(a, 300, 200, "split");
    struct widget *sp = splitpane_new(win, 0);
    struct widget *l = label_new(sp, "left"), *r = label_new(sp, "right");
    window_paint(win);
    CHECK(l->w > 100 && r->x == l->w + 6, "halves: %d and %d at %d", l->w, r->w, r->x);
    widget_abs(sp, &ax, &ay);
    struct wmsg d = mouse_msg(win, WMOUSE_DOWN, ax + l->w + 3, ay + 50, 1);
    window_message(win, &d);
    struct wmsg mv = mouse_msg(win, WMOUSE_MOVE, ax + 60, ay + 50, 1);
    window_message(win, &mv);
    struct wmsg u = mouse_msg(win, WMOUSE_UP, ax + 60, ay + 50, 0);
    window_message(win, &u);
    window_paint(win);
    CHECK(l->w >= 55 && l->w <= 60, "divider dragged: %d", l->w);
    window_close(win);

    win = app_window(a, 300, 200, "menu");
    struct widget *bar = menubar_new(win);
    struct widget *file = menu_new(bar, "File");
    struct widget *it = menu_add(file, "Quit", NULL);
    menu_add_separator(file);
    menu_add(file, "Other", NULL);
    widget_connect(it, "clicked", on_click, NULL);
    window_paint(win);
    widget_abs(bar, &ax, &ay);
    click(win, ax + 10, ay + 5);
    CHECK(window_state_of(win)->popup != NULL && bar->value == 0, "menu opened");
    if (window_state_of(win)->popup) {
        struct widget *pp = window_state_of(win)->popup;
        window_paint(win);
        click(win, pp->x + 10, pp->y + 6);
        CHECK(clicks == 1 && window_state_of(win)->popup == NULL, "item activated and menu closed");
    }
    window_close(win);
}

/* A tree model over a static table: rows 0..9, rows 1 and 2 are
 * children of 0, row 3 is a child of 2. */
static int tm_parent[10] = { -1, 0, 0, 2, -1, -1, -1, -1, -1, -1 };
static int tm_rows(struct model *m, int parent)
{
    int n = 0;
    for (int i = 0; i < 10; i++) n += tm_parent[i] == parent;
    return n;
}
static int tm_child(struct model *m, int parent, int index)
{
    for (int i = 0; i < 10; i++)
        if (tm_parent[i] == parent && index-- == 0)
            return i;
    return -1;
}
static int tm_columns(struct model *m) { return 2; }
static const char *tm_cell(struct model *m, int row, int col, char *buf, size_t size)
{
    snprintf(buf, size, col == 0 ? "row %d" : "%d", row * (col + 1));
    return buf;
}
static const char *tm_header(struct model *m, int col) { return col == 0 ? "Name" : "Value"; }
static int sorted_col = -1, sorted_desc;
static void tm_sort(struct model *m, int col, int desc) { sorted_col = col; sorted_desc = desc; }

static void test_models(struct app *a)
{
    static struct model m = { tm_rows, tm_child, tm_columns, tm_cell, tm_header, tm_sort, NULL };
    struct widget *win = app_window(a, 300, 300, "tree");
    struct widget *tree = treeview_new(win);
    view_set_model(tree, &m);
    widget_connect(tree, "selected", on_select, NULL);
    window_paint(win);
    CHECK(view_visible_rows(tree) == 7, "collapsed tree shows the roots: %d", view_visible_rows(tree));
    treeview_expand(tree, 0, 1);
    CHECK(view_visible_rows(tree) == 9 && view_row_at(tree, 1) == 1 && view_row_at(tree, 3) == 4, "expanded row 0: %d rows", view_visible_rows(tree));
    treeview_expand(tree, 2, 1);
    CHECK(view_visible_rows(tree) == 10 && view_row_at(tree, 3) == 3, "nested expansion");
    widget_focus(tree);
    struct wmsg end = key_msg(win, KEY_END, 0, 0);
    window_message(win, &end);
    CHECK(tree->value == 9 && last_value == 9, "End selects the last row id: %d", tree->value);
    struct wmsg home = key_msg(win, KEY_HOME, 0, 0);
    window_message(win, &home);
    struct wmsg left = key_msg(win, KEY_LEFT, 0, 0);
    window_message(win, &left);
    CHECK(!treeview_is_expanded(tree, 0) && view_visible_rows(tree) == 7, "Left collapses");
    window_close(win);

    win = app_window(a, 300, 300, "table");
    struct widget *table = table_new(win);
    view_set_model(table, &m);
    window_paint(win);
    CHECK(view_visible_rows(table) == 7 && table_column_width(table, 1) == 100, "table rows and default widths");
    int ax, ay;
    widget_abs(table, &ax, &ay);
    click(win, ax + 30, ay + 10);
    CHECK(sorted_col == 0 && !sorted_desc, "header click sorts column 0");
    click(win, ax + 30, ay + 10);
    CHECK(sorted_desc, "second click reverses the order");
    struct wmsg d = mouse_msg(win, WMOUSE_DOWN, ax + 101, ay + 10, 1);
    window_message(win, &d);
    struct wmsg mv = mouse_msg(win, WMOUSE_MOVE, ax + 141, ay + 10, 1);
    window_message(win, &mv);
    struct wmsg u = mouse_msg(win, WMOUSE_UP, ax + 141, ay + 10, 0);
    window_message(win, &u);
    CHECK(table_column_width(table, 0) == 140, "column resized by dragging: %d", table_column_width(table, 0));
    click(win, ax + 30, ay + 24 + 1 + 3 * (app_theme(a)->font->height + 6) + 5);
    CHECK(table->value == 6, "fourth root row (id 6) selected by click: %d", table->value);
    /* Scrolling to the last row of a table two rows high leaves the
     * selection unchanged. */
    int saved_h = table->h;
    table->h = 24 + 2 + 2 * (widget_theme(table)->font->height + 6);
    view_scroll_to(table, view_row_at(table, 6));
    CHECK(view_scroll_position(table) == 5 && table->value == 6, "scrolled to the last row: first %d, selected %d",
          view_scroll_position(table), table->value);
    view_scroll_to(table, view_row_at(table, 0));
    CHECK(view_scroll_position(table) == 0, "scrolled back to the first row: %d", view_scroll_position(table));
    table->h = saved_h;
    window_close(win);
}

static void test_editor(struct app *a)
{
    struct widget *win = app_window(a, 400, 300, "editor");
    struct widget *ed = editor_new(win);
    window_paint(win);
    widget_focus(ed);
    type_text(win, "hello\nworld");
    char *t = editor_text(ed);
    CHECK(strcmp(t, "hello\nworld") == 0 && editor_line_count(ed) == 2, "typed two lines: '%s'", t);
    free(t);
    int l, c;
    editor_cursor(ed, &l, &c);
    CHECK(l == 1 && c == 5, "cursor at the end: %d,%d", l, c);
    struct wmsg bs = key_msg(win, KEY_BACKSPACE, '\b', 0);
    window_message(win, &bs);
    window_message(win, &bs);
    t = editor_text(ed);
    CHECK(strcmp(t, "hello\nwor") == 0, "backspace: '%s'", t);
    free(t);
    CHECK(editor_undo(ed) && editor_undo(ed), "two undos");
    t = editor_text(ed);
    CHECK(strcmp(t, "hello\nworld") == 0, "undo restored: '%s'", t);
    free(t);
    CHECK(editor_undo(ed) && editor_undo(ed) && editor_undo(ed), "undo the typing: a word, the newline, a word");
    t = editor_text(ed);
    CHECK(strcmp(t, "") == 0, "typed text undone in three groups: '%s'", t);
    free(t);
    CHECK(editor_redo(ed) && editor_redo(ed) && editor_redo(ed), "redo");
    t = editor_text(ed);
    CHECK(strcmp(t, "hello\nworld") == 0, "redo restored: '%s'", t);
    free(t);
    /* Selection across lines, cut, paste. */
    editor_goto(ed, 0, 3);
    struct wmsg right = key_msg(win, KEY_RIGHT, 0, WMOD_SHIFT);
    for (int i = 0; i < 5; i++)
        window_message(win, &right);
    struct wmsg cut = key_msg(win, KEY_X, 24, WMOD_CTRL);
    window_message(win, &cut);
    t = editor_text(ed);
    CHECK(strcmp(t, "helrld") == 0, "cut across lines: '%s'", t);
    free(t);
    editor_goto(ed, 0, 0);
    struct wmsg paste = key_msg(win, KEY_V, 22, WMOD_CTRL);
    window_message(win, &paste);
    t = editor_text(ed);
    CHECK(strcmp(t, "lo\nwohelrld") == 0, "pasted at the start: '%s'", t);
    free(t);
    /* Search. */
    editor_goto(ed, 0, 0);
    CHECK(editor_find(ed, "hel", 1), "find forward");
    editor_cursor(ed, &l, &c);
    CHECK(l == 1 && c == 5, "match selected up to 1,5: %d,%d", l, c);
    CHECK(!editor_find(ed, "zzz", 1), "missing text not found");
    /* Wrapping: a long line produces several visual rows; Down moves
     * within the line. */
    editor_set_text(ed, "one two three four five six seven eight nine ten eleven twelve thirteen fourteen fifteen sixteen seventeen");
    editor_set_wrap(ed, 1);
    window_paint(win);
    editor_goto(ed, 0, 0);
    struct wmsg down = key_msg(win, KEY_DOWN, 0, 0);
    window_message(win, &down);
    editor_cursor(ed, &l, &c);
    CHECK(l == 0 && c > 0, "Down moves along the wrapped line: %d,%d", l, c);
    /* Highlighter classes. */
    unsigned char cls[64];
    int state = 0;
    const char *src = "int x = 42; /* c";
    highlight_c(src, (int)strlen(src), cls, &state, NULL);
    CHECK(cls[0] == HL_KEYWORD && cls[4] == HL_NORMAL && cls[8] == HL_NUMBER && cls[12] == HL_COMMENT && state == 1,
          "C classes %d %d %d %d state %d", cls[0], cls[4], cls[8], cls[12], state);
    src = "x */ y";
    highlight_c(src, (int)strlen(src), cls, &state, NULL);
    CHECK(cls[0] == HL_COMMENT && cls[5] == HL_NORMAL && state == 0, "block comment ends: %d %d", cls[0], cls[5]);
    src = "echo $HOME # hi";
    highlight_sh(src, (int)strlen(src), cls, &state, NULL);
    CHECK(cls[0] == HL_KEYWORD && cls[5] == HL_PREPROC && cls[11] == HL_COMMENT, "shell classes %d %d %d", cls[0], cls[5], cls[11]);
    window_close(win);
}

void run_widget2_tests(void)
{
    struct app *a = app_create_detached();
#define T(f) do { fprintf(stderr, "  %s\n", #f); f(a); } while (0)
    T(test_controls);
    T(test_containers);
    T(test_models);
    T(test_editor);
    app_step(a, 0);
    app_destroy(a);
}

/* The gedit widget tree typed into on the host (a hang here showed up
 * only in the boot test). */
static struct widget *g_status, *g_pos, *g_editor;
static int g_cursor(struct widget *w, void *args, void *arg)
{
    int l, c;
    editor_cursor(g_editor, &l, &c);
    char s[48];
    snprintf(s, sizeof s, "line %d, column %d", l + 1, c + 1);
    widget_set_text(g_pos, s);
    return 0;
}
static int g_changed(struct widget *w, void *args, void *arg)
{
    widget_set_text(g_status, "");
    return g_cursor(w, args, arg);
}

void run_gedit_tree_test(void)
{
    struct app *a = app_create_detached();
    struct widget *win = app_window(a, 600, 420, "gedit");
    struct widget *bar = menubar_new(win);
    struct widget *file = menu_new(bar, "File");
    menu_add(file, "New", "new");
    menu_add_separator(file);
    menu_add(file, "Quit", "quit");
    struct widget *tools = toolbar_new(win);
    toolbar_add(tools, "new", "New");
    struct widget *save = toolbar_add(tools, "save", "Save");
    widget_set_accel(save, 0x1f, WMOD_CTRL);
    checkbox_new(tools, "Wrap");
    checkbox_new(tools, "Numbers");
    g_editor = editor_new(win);
    editor_set_highlighter(g_editor, highlight_c, NULL);
    widget_connect(g_editor, "changed", g_changed, NULL);
    widget_connect(g_editor, "cursor", g_cursor, NULL);
    struct widget *sb = statusbar_new(win);
    g_status = statusbar_add(sb, 1);
    g_pos = statusbar_add(sb, 0);
    g_cursor(NULL, NULL, NULL);
    widget_focus(g_editor);
    window_paint(win);
    type_text(win, "int x;");
    window_paint(win);
    char *t = editor_text(g_editor);
    CHECK(strcmp(t, "int x;") == 0, "gedit tree typed: '%s'", t);
    free(t);
    CHECK(strcmp(widget_text(g_pos), "line 1, column 7") == 0, "status shows the cursor: '%s'", widget_text(g_pos));
    struct wmsg s = key_msg(win, KEY_S, 19, WMOD_CTRL);
    window_message(win, &s);
    window_close(win);
    app_step(a, 0);
    app_destroy(a);
}
