/* The graph widget (X1 of docs/plan/x12settings.md). */
#include <gui/app.h>
#include <string.h>
#include "check.h"

struct app *app_create_detached(void);

/* Pixels of the colour in the window's surface. */
static int count_color(struct widget *win, uint32_t color)
{
    struct surface *s = &window_state_of(win)->win->surf;
    int n = 0;
    for (int y = 0; y < s->height; y++)
        for (int x = 0; x < s->width; x++)
            n += (s->pixels[(size_t)y * s->stride + x] & 0xffffff) == (color & 0xffffff);
    return n;
}

void run_graph_tests(void)
{
    CHECK(graph_nice_max(0, 1) == 1 && graph_nice_max(7, 1) == 10 && graph_nice_max(100, 1) == 100 &&
              graph_nice_max(101, 1) == 200 && graph_nice_max(1500, 1024) == 2000 &&
              graph_nice_max(1024, 1024) == 1024 && graph_nice_max(12345, 1) == 20000 &&
              graph_nice_max(400, 1000) == 1000,
          "graph_nice_max rounds up to 1, 2 or 5 times a power of ten");

    struct app *a = app_create_detached();
    struct widget *win = app_window(a, 240, 120, "graph");
    struct widget *g = graph_new(win, 3);
    widget_set_stretch(g, 1, 1);
    CHECK(graph_add_series(g, "a", 0, GRAPH_AREA) == 0, "first series");
    graph_set_scale(g, 1, 1);
    long v = 30;
    graph_push(g, &v);
    v = 70;
    graph_push(g, &v);
    CHECK(graph_scale(g) == 100, "automatic scale of 30 and 70: %ld", graph_scale(g));
    v = 700;
    graph_push(g, &v);
    CHECK(graph_scale(g) == 1000, "automatic scale with 700: %ld", graph_scale(g));
    for (int i = 0; i < 3; i++) {
        v = 1;
        graph_push(g, &v);
    }
    CHECK(graph_scale(g) == 1, "the oldest samples leave the graph: scale %ld", graph_scale(g));
    graph_set_scale(g, 1000, 0);
    CHECK(graph_scale(g) == 1000, "fixed scale");

    /* A level line at half height in the accent colour, and none for a
     * series of style GRAPH_TEXT. */
    uint32_t accent = app_theme(a)->color[TC_ACCENT];
    for (int i = 0; i < 3; i++) {
        v = 500;
        graph_push(g, &v);
    }
    window_paint(win);
    int line = count_color(win, accent);
    CHECK(line >= g->w - 4 && line < 3 * g->w, "the line has about the width of the graph: %d pixels, width %d",
          line, g->w);
    graph_set_style(g, 0, GRAPH_TEXT);
    window_paint(win);
    CHECK(count_color(win, accent) == 0, "a text series draws no line: %d pixels", count_color(win, accent));
    CHECK(graph_add_series(g, "b", 0, GRAPH_LINE) == 1 && graph_add_series(g, "c", 0, GRAPH_LINE) == 2 &&
              graph_add_series(g, "d", 0, GRAPH_LINE) == 3 && graph_add_series(g, "e", 0, GRAPH_LINE) == -1,
          "at most GRAPH_SERIES series");
    window_close(win);
}
