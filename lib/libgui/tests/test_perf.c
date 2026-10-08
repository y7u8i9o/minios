/* K2 of docs/plan/widgets.md: input that changes no pixel causes no paint,
 * a label update repaints the label alone, and a scroll step of a scroll
 * area needs no layout. The counters come from gui_get_stats of the fake
 * client. */
#include <gui/app.h>
#include <gui/model.h>
#include <stdio.h>
#include "check.h"
#include "events.h"
#include "fake.h"

struct app *app_create_detached(void);

static struct gui_stats stats(void)
{
    struct gui_stats s;
    gui_get_stats(&s);
    return s;
}

static void send(struct widget *win, int kind, struct widget *w, int x, int y, int buttons)
{
    int ax, ay;
    widget_abs(w, &ax, &ay);
    struct wmsg m = mouse_msg(win, kind, ax + x, ay + y, buttons);
    window_message(win, &m);
}

/* Sends the input and paints. Returns 1 when the paint changed nothing:
 * no paint of the window and no damage. */
static int unchanged(struct widget *win, int kind, struct widget *w, int x, int y, int buttons)
{
    struct gui_stats before = stats();
    int damage = fake_damage_count;
    send(win, kind, w, x, y, buttons);
    if (kind == WMOUSE_DOWN)
        send(win, WMOUSE_UP, w, x, y, 0);
    window_paint(win);
    return stats().paints == before.paints && fake_damage_count == damage;
}

static int rows(struct model *m, int parent) { return parent < 0 ? 60 : 0; }
static int child(struct model *m, int parent, int index) { return index; }
static int columns(struct model *m) { return 1; }
static const char *cell(struct model *m, int row, int col, char *buf, size_t size)
{
    snprintf(buf, size, "row %d", row);
    return buf;
}
static struct model model = { rows, child, columns, cell, NULL };

/* The table and the list view at both ends of their range. The track is
 * the scroll bar width at the right edge. */
static void test_ends(struct app *a)
{
    int sb = theme_px(app_theme(a), TM_SCROLLBAR);
    struct widget *win = app_window(a, 300, 300, "ends");
    struct widget *table = table_new(win);
    view_set_model(table, &model);
    struct widget *list = listview_new(win);
    for (int i = 0; i < 60; i++)
        listview_add(list, "item");
    window_paint(win);
    struct { struct widget *w; int top; const char *name; } views[] = { { table, 24, "table" }, { list, 0, "list" } };
    for (int i = 0; i < 2; i++) {
        struct widget *w = views[i].w;
        int tx = w->w - sb / 2;
        /* A press focuses the view, and the focus ring is a change. */
        widget_focus(w);
        window_paint(win);
        CHECK(unchanged(win, WMOUSE_WHEEL, w, 40, w->h / 2, -1), "%s: a wheel step up at the top", views[i].name);
        CHECK(unchanged(win, WMOUSE_DOWN, w, tx, views[i].top, 1), "%s: a page click above the thumb at the top",
              views[i].name);
        /* To the end with the wheel. */
        for (int k = 0; k < 30; k++) {
            send(win, WMOUSE_WHEEL, w, 40, w->h / 2, 1);
            window_paint(win);
        }
        CHECK(unchanged(win, WMOUSE_WHEEL, w, 40, w->h / 2, 1), "%s: a wheel step down at the end", views[i].name);
        CHECK(unchanged(win, WMOUSE_DOWN, w, tx, w->h - 1, 1), "%s: a page click below the thumb at the end",
              views[i].name);
    }
    window_close(win);
}

static void test_status(struct app *a)
{
    struct widget *win = app_window(a, 400, 200, "status");
    canvas_new(win);
    struct widget *bar = statusbar_new(win);
    struct widget *label = statusbar_add(bar, 1);
    widget_set_text(label, "Ready");
    widget_set_text(statusbar_add(bar, 0), "Line 1");
    window_paint(win);
    struct gui_stats before = stats();
    int damage = fake_damage_count;
    widget_set_text(label, "Saved the file");
    window_paint(win);
    struct gui_stats after = stats();
    int lx, ly;
    widget_abs(label, &lx, &ly);
    CHECK(after.widget_paints - before.widget_paints <= 2, "a status label update paints %llu widgets",
          (unsigned long long)(after.widget_paints - before.widget_paints));
    CHECK(fake_damage_count == damage + 1 && fake_last_damage.x == lx && fake_last_damage.y == ly &&
              fake_last_damage.w == label->w && fake_last_damage.h == label->h,
          "the damage is the label: %d,%d %dx%d, label %d,%d %dx%d", fake_last_damage.x, fake_last_damage.y,
          fake_last_damage.w, fake_last_damage.h, lx, ly, label->w, label->h);
    window_close(win);
}

static void test_scroll_area(struct app *a)
{
    struct widget *win = app_window(a, 300, 300, "area");
    struct widget *area = scrollarea_new(win);
    for (int i = 0; i < 200; i++)
        label_new(area->user, "a label");
    window_paint(win);
    struct widget *content = area->user;
    int y0 = content->y;
    struct gui_stats before = stats();
    send(win, WMOUSE_WHEEL, area, 40, 40, 1);
    window_paint(win);
    struct gui_stats after = stats();
    CHECK(content->y < y0, "the wheel scrolls the content: %d to %d", y0, content->y);
    CHECK(after.layouts == before.layouts, "a scroll step adds %llu layouts",
          (unsigned long long)(after.layouts - before.layouts));
    CHECK(after.paints == before.paints + 1, "a scroll step paints the window once");
    CHECK(after.widget_paints - before.widget_paints < 60, "a scroll step paints %llu widgets",
          (unsigned long long)(after.widget_paints - before.widget_paints));
    window_close(win);
}

void run_perf_tests(void)
{
    struct app *a = app_create_detached();
    test_ends(a);
    test_status(a);
    test_scroll_area(a);
    app_destroy(a);
}
