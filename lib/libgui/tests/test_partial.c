/* K3 of docs/plan/widgets.md: partial repaints, scrolling by copy and the
 * editor work per edit. */
#include <gui/app.h>
#include <gui/model.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "check.h"
#include "events.h"
#include "fake.h"

struct app *app_create_detached(void);

static void send(struct widget *win, int kind, struct widget *w, int x, int y, int buttons)
{
    int ax, ay;
    widget_abs(w, &ax, &ay);
    struct wmsg m = mouse_msg(win, kind, ax + x, ay + y, buttons);
    window_message(win, &m);
}

static uint64_t counter(int which)
{
    struct gui_stats s;
    gui_get_stats(&s);
    return which == 0 ? s.painted_pixels : s.text_shapes;
}

/* One keystroke repaints the field from the edit to its end. */
static void test_keystroke(struct app *a)
{
    struct widget *win = app_window(a, 400, 100, "field");
    struct widget *field = textfield_new(win, "");
    widget_set_hint(field, 300, 0);
    widget_set_stretch(field, 0, 0);
    widget_focus(field);
    window_paint(win);
    type_text(win, "hello");
    window_paint(win);
    uint64_t before = counter(0);
    int x = 4 + widget_text_width(field, NULL, "hello", -1);
    type_text(win, "w");
    window_paint(win);
    uint64_t pixels = counter(0) - before, limit = (uint64_t)field->h * (uint64_t)(field->w - x + 2);
    CHECK(pixels > 0 && pixels <= limit, "a keystroke damages %llu pixels, at most %llu: %d,%d %dx%d, field at %d %dx%d",
          (unsigned long long)pixels, (unsigned long long)limit, fake_last_damage.x, fake_last_damage.y,
          fake_last_damage.w, fake_last_damage.h, field->x, field->w, field->h);
    window_close(win);
}

static int rows(struct model *m, int parent) { return parent < 0 ? 200 : 0; }
static int child(struct model *m, int parent, int index) { return index; }
static int columns(struct model *m) { return 2; }
static int cells;
static const char *cell(struct model *m, int row, int col, char *buf, size_t size)
{
    cells++;
    snprintf(buf, size, "row %d col %d", row, col);
    return buf;
}
static struct model model = { rows, child, columns, cell, NULL };

/* The pixels of the window after a paint by copy equal those of a full
 * repaint. The function returns 1, or prints the first different pixel
 * and returns 0. */
static int same_as_full(struct widget *win)
{
    struct surface *s = &window_state_of(win)->win->surf;
    size_t n = (size_t)s->stride * s->height;
    uint32_t *copy = malloc(n * 4);
    memcpy(copy, s->pixels, n * 4);
    widget_invalidate(win);
    window_paint(win);
    size_t i = 0;
    while (i < n && copy[i] == s->pixels[i])
        i++;
    if (i < n)
        printf("first different device pixel at %d,%d: %06x after the copy, %06x repainted\n", (int)(i % s->stride),
               (int)(i / s->stride), copy[i] & 0xffffff, s->pixels[i] & 0xffffff);
    free(copy);
    return i == n;
}

static void test_copy(struct app *a, int scale)
{
    fake_scale = scale;
    const struct theme *t = app_theme(a);
    struct widget *win = app_window(a, 360, 400, "copy");
    struct widget *table = table_new(win);
    view_set_model(table, &model);
    widget_set_stretch(table, 1, 0);
    /* 10 rows exactly, so a step of 3 rows exposes 3 rows. */
    widget_set_hint(table, 0, 2 + 24 + 10 * (t->font->height + 6));
    struct widget *list = listview_new(win);
    for (int i = 0; i < 100; i++) {
        char item[16];
        snprintf(item, sizeof item, "item %d", i);
        listview_add(list, item);
    }
    window_paint(win);
    cells = 0;
    send(win, WMOUSE_WHEEL, table, 40, 60, 1);
    window_paint(win);
    CHECK(cells <= 3 * 2, "scale %d: a wheel step of 3 rows calls cell %d times", scale, cells);
    CHECK(same_as_full(win), "scale %d: the table after a copy equals a full repaint", scale);
    send(win, WMOUSE_WHEEL, list, 40, 20, 1);
    window_paint(win);
    send(win, WMOUSE_WHEEL, list, 40, 20, -1);
    send(win, WMOUSE_WHEEL, list, 40, 20, 2);
    window_paint(win);
    CHECK(same_as_full(win), "scale %d: the list after copies equals a full repaint", scale);
    window_close(win);

    win = app_window(a, 300, 300, "area");
    struct widget *area = scrollarea_new(win);
    for (int i = 0; i < 100; i++) {
        char text[16];
        snprintf(text, sizeof text, "label %d", i);
        label_new(area->user, text);
    }
    window_paint(win);
    for (int k = 0; k < 3; k++) {
        send(win, WMOUSE_WHEEL, area, 40, 40, 4);
        window_paint(win);
    }
    CHECK(same_as_full(win), "scale %d: the scroll area after copies equals a full repaint", scale);
    window_close(win);
    fake_scale = 1;
}

static int hl_lines;
static void counting_hl(const char *line, int len, unsigned char *classes, int *state, void *arg)
{
    hl_lines++;
    highlight_c(line, len, classes, state, arg);
}

static void test_editor(struct app *a)
{
    struct widget *win = app_window(a, 500, 400, "editor");
    struct widget *ed = editor_new(win);
    size_t size = 4000 * 40;
    char *text = malloc(size), *at = text;
    for (int i = 0; i < 4000; i++)
        at += sprintf(at, i % 3 ? "int value_%d = %d;\n" : "/* comment %d %d */\n", i, i);
    editor_set_text(ed, text);
    free(text);
    editor_set_highlighter(ed, counting_hl, NULL);
    editor_goto(ed, 3499, 0);
    widget_focus(ed);
    window_paint(win);
    type_text(win, "x");
    window_paint(win);
    hl_lines = 0;
    type_text(win, "y");
    window_paint(win);
    int vis = (ed->h - 2) / (app_theme(a)->font->height + 2);
    CHECK(hl_lines <= vis + 2, "a paint at line 3500 runs the highlighter for %d lines, %d visible", hl_lines, vis);
    CHECK(strcmp(editor_line(ed, 3499), "xy/* comment 3499 3499 */") == 0 || strncmp(editor_line(ed, 3499), "xy", 2) == 0,
          "the typed text: %s", editor_line(ed, 3499));

    /* With wrapping, one keystroke wraps the edited line again only. */
    editor_set_wrap(ed, 1);
    window_paint(win);
    uint64_t before = counter(1);
    type_text(win, "z");
    uint64_t shapes = counter(1) - before;
    CHECK(shapes <= 4, "one keystroke shapes %llu texts before the paint", (unsigned long long)shapes);
    window_paint(win);
    char *all = editor_text(ed);
    CHECK(all && strstr(all, "xyz") != NULL, "the text after the edits");
    free(all);
    window_close(win);
}

void run_partial_tests(void)
{
    struct app *a = app_create_detached();
    test_keystroke(a);
    test_copy(a, 1);
    test_copy(a, 2);
    test_editor(a);
    app_destroy(a);
}
