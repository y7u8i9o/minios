/* Host unit tests of the M21 framework: layout, signals, partial
 * redraw, focus traversal and text editing. */
#include <gui/app.h>
#include <stdio.h>
#include <string.h>
#include "fake.h"

struct app *app_create_detached(void);
#include "check.h"

static int count_a, count_b;
static int handler_a(struct widget *w, void *args, void *arg) { count_a++; return 0; }
static int handler_b(struct widget *w, void *args, void *arg) { count_b++; return 1; }
static int handler_never(struct widget *w, void *args, void *arg) { CHECK(0, "handler after a consuming one ran"); return 0; }

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

static void type_text(struct widget *win, const char *s)
{
    for (; *s; s++) {
        struct wmsg m = key_msg(win, 0, *s, 0);
        window_message(win, &m);
    }
}

static void test_box_layout(struct app *a)
{
    struct widget *win = app_window(a, 200, 100, "box");
    struct widget *l1 = label_new(win, "one");
    struct widget *l2 = label_new(win, "two");
    widget_set_stretch(l2, 0, 1);
    window_paint(win);
    const struct theme *t = app_theme(a);
    int pad = theme_px(t, TM_PADDING), sp = theme_px(t, TM_SPACING);
    CHECK(l1->x == pad && l1->y == pad && l1->w == 200 - 2 * pad, "label one at %d,%d %dx%d", l1->x, l1->y, l1->w, l1->h);
    CHECK(l2->y == l1->y + l1->h + sp, "label two below one: %d", l2->y);
    CHECK(l2->y + l2->h == 100 - pad, "stretched label fills to the bottom: %d", l2->y + l2->h);
    window_close(win);
}

static void test_grid_layout(struct app *a)
{
    struct widget *win = app_window(a, 300, 200, "grid");
    struct widget *g = grid_new(win);
    widget_set_stretch(g, 1, 1);
    struct widget *c00 = button_new(g, "a"), *c01 = button_new(g, "b"), *c10 = button_new(g, "wide");
    widget_set_grid(c00, 0, 0, 1, 1);
    widget_set_grid(c01, 0, 1, 1, 1);
    widget_set_grid(c10, 1, 0, 1, 2);
    grid_set_stretch(g, -1, 1, 1);
    grid_set_stretch(g, 1, -1, 1);
    window_paint(win);
    CHECK(c01->x > c00->x + c00->w, "second column right of the first");
    CHECK(c10->y > c00->y + c00->h, "second row below the first");
    CHECK(c10->w == c01->x + c01->w - c00->x, "spanning child covers both columns: %d vs %d", c10->w, c01->x + c01->w - c00->x);
    CHECK(c01->w > c00->w, "stretched column is wider: %d vs %d", c01->w, c00->w);
    CHECK(c10->y + c10->h > 150, "stretched row takes the extra height: %d", c10->y + c10->h);
    window_close(win);
}

static void test_signals(struct app *a)
{
    struct widget *win = app_window(a, 100, 100, "sig");
    struct widget *b = button_new(win, "x");
    widget_connect(b, "clicked", handler_a, NULL);
    widget_connect(b, "clicked", handler_b, NULL);
    widget_connect(b, "clicked", handler_never, NULL);
    struct sig_click c = { 1, 0, 0 };
    int consumed = widget_emit(b, "clicked", &c);
    CHECK(consumed && count_a == 1 && count_b == 1, "handlers ran in order: %d %d", count_a, count_b);
    widget_emit(b, "other", NULL);
    CHECK(count_a == 1, "unrelated signal did not run the handler");
    window_close(win);
}

static void test_partial_redraw(struct app *a)
{
    struct widget *win = app_window(a, 300, 200, "redraw");
    struct widget *b1 = button_new(win, "first");
    struct widget *b2 = button_new(win, "second");
    struct widget *l = listview_new(win);
    listview_add(l, "item");
    struct rect r = window_paint(win);
    CHECK(r.x == 0 && r.y == 0 && r.w == 300 && r.h == 200, "first paint covers the window: %d,%d %dx%d", r.x, r.y, r.w, r.h);
    r = window_paint(win);
    CHECK(rect_empty(r), "nothing to paint without changes");
    widget_invalidate(b2);
    r = window_paint(win);
    CHECK(r.x == b2->x && r.y == b2->y && r.w == b2->w && r.h == b2->h, "only the invalidated button repainted: %d,%d %dx%d", r.x, r.y, r.w, r.h);
    CHECK(fake_last_damage.y == b2->y, "damage sent to the server matches");
    widget_set_text(b1, "first!");
    r = window_paint(win);
    CHECK(r.w == 300 && r.h == 200, "a text change relayouts and repaints the window");
    window_close(win);
}

static int focus_changes;
static int on_focus_click(struct widget *w, void *args, void *arg) { focus_changes++; return 1; }

static void test_focus(struct app *a)
{
    struct widget *win = app_window(a, 200, 200, "focus");
    struct widget *b1 = button_new(win, "&One"), *b2 = button_new(win, "&Two");
    struct widget *f = textfield_new(win, "");
    widget_connect(b2, "clicked", on_focus_click, NULL);
    window_paint(win);
    struct wmsg tab = key_msg(win, 0x0f, '\t', 0);
    window_message(win, &tab);
    CHECK(widget_focused(win) == b1, "Tab focuses the first button");
    window_message(win, &tab);
    CHECK(widget_focused(win) == b2, "Tab moves to the second button");
    window_message(win, &tab);
    CHECK(widget_focused(win) == f, "Tab moves to the field");
    window_message(win, &tab);
    CHECK(widget_focused(win) == b1, "Tab wraps around");
    struct wmsg back = key_msg(win, 0x0f, '\t', WMOD_SHIFT);
    window_message(win, &back);
    CHECK(widget_focused(win) == f, "Shift+Tab goes backwards");
    struct wmsg alt = key_msg(win, 0x14, 't', WMOD_ALT);
    window_message(win, &alt);
    CHECK(focus_changes == 1, "Alt+T activates the mnemonic");
    struct wmsg click = mouse_msg(win, WMOUSE_DOWN, b2->x + 5, b2->y + 5, 1);
    window_message(win, &click);
    struct wmsg up = mouse_msg(win, WMOUSE_UP, b2->x + 5, b2->y + 5, 0);
    window_message(win, &up);
    CHECK(focus_changes == 2 && widget_focused(win) == b2, "a click focuses and activates the button");
    window_close(win);
}

static const char *last_text;
static int last_change(struct widget *w, void *args, void *arg) { last_text = ((struct sig_change *)args)->text; return 0; }

static void test_textfield(struct app *a)
{
    struct widget *win = app_window(a, 300, 100, "text");
    struct widget *f = textfield_new(win, "");
    widget_connect(f, "changed", last_change, NULL);
    window_paint(win);
    widget_focus(f);
    type_text(win, "hello");
    CHECK(strcmp(widget_text(f), "hello") == 0 && last_text && strcmp(last_text, "hello") == 0, "typed text '%s'", widget_text(f));
    struct wmsg left = key_msg(win, 0xcb, 0, WMOD_SHIFT);
    window_message(win, &left);
    window_message(win, &left);
    struct wmsg cut = key_msg(win, 0x2d, 24, WMOD_CTRL);
    window_message(win, &cut);
    CHECK(strcmp(widget_text(f), "hel") == 0, "cut selection: '%s'", widget_text(f));
    struct wmsg home = key_msg(win, 0xc7, 0, 0);
    window_message(win, &home);
    struct wmsg paste = key_msg(win, 0x2f, 22, WMOD_CTRL);
    window_message(win, &paste);
    CHECK(strcmp(widget_text(f), "lohel") == 0, "paste at the start: '%s'", widget_text(f));
    struct wmsg bs = key_msg(win, 0x0e, '\b', 0);
    window_message(win, &bs);
    CHECK(strcmp(widget_text(f), "lhel") == 0, "backspace: '%s'", widget_text(f));
    struct rect r = window_paint(win);
    CHECK(r.y == f->y && r.h == f->h, "typing repaints only the field: %d,%d %dx%d", r.x, r.y, r.w, r.h);
    window_close(win);
}

static void test_listview(struct app *a)
{
    struct widget *win = app_window(a, 200, 100, "list");
    struct widget *l = listview_new(win);
    for (int i = 0; i < 20; i++) {
        char s[8];
        snprintf(s, sizeof s, "%d", i);
        listview_add(l, s);
    }
    window_paint(win);
    widget_focus(l);
    struct wmsg end = key_msg(win, 0xcf, 0, 0);
    window_message(win, &end);
    CHECK(l->value == 19, "End selects the last item: %d", l->value);
    struct wmsg up = key_msg(win, 0xc8, 0, 0);
    window_message(win, &up);
    CHECK(l->value == 18, "Up moves the selection: %d", l->value);
    CHECK(listview_count(l) == 20 && strcmp(listview_item(l, 3), "3") == 0, "items are kept");
    window_close(win);
}

void run_framework_tests(void)
{
    struct app *a = app_create_detached();
    test_box_layout(a);
    test_grid_layout(a);
    test_signals(a);
    test_partial_redraw(a);
    test_focus(a);
    test_textfield(a);
    test_listview(a);
    app_step(a, 0);
    app_destroy(a);
}
