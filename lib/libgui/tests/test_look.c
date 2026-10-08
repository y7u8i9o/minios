/* K5 of docs/plan/widgets.md: hover, the disabled look and the marks of
 * check boxes, radio buttons and combo boxes. */
#include <gui/app.h>
#include <gui/model.h>
#include <stdio.h>
#include "check.h"
#include "events.h"
#include "fake.h"

struct app *app_create_detached(void);

static uint32_t pixel(struct widget *win, int x, int y)
{
    struct surface *s = &window_state_of(win)->win->surf;
    return s->pixels[(size_t)y * s->stride + x] & 0xffffff;
}

/* The number of pixels of color in the rectangle of w. */
static int count(struct widget *win, struct widget *w, uint32_t color)
{
    int ax, ay, n = 0;
    widget_abs(w, &ax, &ay);
    for (int y = ay; y < ay + w->h; y++)
        for (int x = ax; x < ax + w->w; x++)
            n += pixel(win, x, y) == color;
    return n;
}

static int rows(struct model *m, int parent) { return parent < 0 ? 30 : 0; }
static int child(struct model *m, int parent, int index) { return index; }
static int columns(struct model *m) { return 1; }
static const char *cell(struct model *m, int row, int col, char *buf, size_t size)
{
    snprintf(buf, size, "row %d", row);
    return buf;
}
static struct model model = { rows, child, columns, cell, NULL };

static uint64_t painted(void)
{
    struct gui_stats s;
    gui_get_stats(&s);
    return s.painted_pixels;
}

static void move(struct widget *win, struct widget *w, int x, int y)
{
    int ax, ay;
    widget_abs(w, &ax, &ay);
    struct wmsg m = mouse_msg(win, WMOUSE_MOVE, ax + x, ay + y, 0);
    window_message(win, &m);
}

/* A move of the pointer from one row to the next repaints the two rows. */
static void test_hover(struct app *a)
{
    const struct theme *t = app_theme(a);
    struct widget *win = app_window(a, 300, 300, "hover");
    struct widget *tree = treeview_new(win);
    view_set_model(tree, &model);
    window_paint(win);
    int lh = t->font->height + theme_px(t, TM_ROW_PAD);
    move(win, tree, 40, 1 + lh + lh / 2);
    window_paint(win);
    CHECK(count(win, tree, t->color[TC_BUTTON_HOVER]) > 0, "the row under the pointer shows the hover colour");
    uint64_t before = painted();
    move(win, tree, 40, 1 + 2 * lh + lh / 2);
    window_paint(win);
    /* The 30 rows overflow the tree, so a row ends at the scroll track. */
    uint64_t rows_px = 2 * (uint64_t)lh * (uint64_t)(tree->w - 2 - theme_px(t, TM_SCROLLBAR));
    CHECK(painted() - before == rows_px, "a hover move repaints two rows: %llu pixels, two rows %llu",
          (unsigned long long)(painted() - before), (unsigned long long)rows_px);
    window_close(win);
}

static int clicked;
static int on_click(struct widget *w, void *args, void *arg)
{
    clicked++;
    return 1;
}

static void test_disabled(struct app *a)
{
    const struct theme *t = app_theme(a);
    struct widget *win = app_window(a, 300, 200, "disabled");
    struct widget *button = button_new(win, "Press");
    struct widget *label = label_new(win, "Label");
    widget_connect(button, "clicked", on_click, NULL);
    widget_set_enabled(button, 0);
    widget_set_enabled(label, 0);
    window_paint(win);
    int bx, by;
    widget_abs(button, &bx, &by);
    click(win, bx + button->w / 2, by + button->h / 2);
    CHECK(clicked == 0, "a disabled button ignores a click");
    CHECK(count(win, button, t->color[TC_TEXT_DISABLED]) > 0 && count(win, label, t->color[TC_TEXT_DISABLED]) > 0,
          "the disabled button and label paint the disabled text colour");
    CHECK(count(win, label, t->color[TC_TEXT]) == 0, "the disabled label has no pixel of the text colour");
    window_close(win);
}

static void test_marks(struct app *a)
{
    const struct theme *t = app_theme(a);
    struct widget *win = app_window(a, 300, 200, "marks");
    struct widget *check = checkbox_new(win, "Check");
    struct widget *radio = radio_new(win, "Radio");
    struct widget *combo = combobox_new(win);
    combobox_add(combo, "Item");
    widget_set_value(check, 1);
    widget_set_value(radio, 1);
    window_paint(win);
    int box = theme_px(t, TM_ICON), x, y;
    widget_abs(radio, &x, &y);
    int cy = y + (radio->h - box) / 2 + box / 2;
    CHECK(pixel(win, x + box / 2, cy) == t->color[TC_SELECTION_TEXT], "the radio dot: %06x",
          pixel(win, x + box / 2, cy));
    CHECK(pixel(win, x + 2, cy) == t->color[TC_ACCENT], "the checked radio disc: %06x", pixel(win, x + 2, cy));
    widget_abs(check, &x, &y);
    int marked = 0, top = y + (check->h - box) / 2;
    for (int j = 0; j < box; j++)
        for (int i = 0; i < box; i++)
            marked += pixel(win, x + i, top + j) == t->color[TC_SELECTION_TEXT];
    CHECK(marked > box / 2, "the check mark: %d pixels", marked);
    CHECK(pixel(win, x + box / 2, top + 2) == t->color[TC_ACCENT], "the checked box is filled with the accent");
    widget_abs(combo, &x, &y);
    int a_w = theme_scale_px(t, 18), ink = 0;
    for (int j = 0; j < combo->h; j++)
        for (int i = combo->w - a_w; i < combo->w; i++)
            ink += pixel(win, x + i, y + j) == t->color[TC_TEXT];
    CHECK(ink > 2, "the chevron of the combo box: %d pixels", ink);
    window_close(win);
}

void run_look_tests(void)
{
    struct app *a = app_create_detached();
    test_hover(a);
    test_disabled(a);
    test_marks(a);
    app_destroy(a);
}
