/* Graph: time series with a heading, a grid and a scale (gui/widget.h). */
#include <gui/app.h>
#include <gui/pixel.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct series {
    char label[64];
    uint32_t color;
    enum graph_style style;
};

struct graph {
    struct widget w;
    int samples, count;                 /* capacity and retained samples */
    int nseries;
    struct series series[GRAPH_SERIES];
    long *values;                       /* samples rows of GRAPH_SERIES, oldest first */
    long max;
    int automatic;
    char title[64], value[64];
    void (*format)(long value, char *buf, size_t size);
};

long graph_nice_max(long v, long minimum)
{
    if (v <= minimum)
        return minimum > 0 ? minimum : 1;
    long step = 1;
    while (step <= v / 10)
        step *= 10;
    if (v <= step)
        return step;
    if (v <= 2 * step)
        return 2 * step;
    if (v <= 5 * step)
        return 5 * step;
    return 10 * step;
}

long graph_scale(const struct widget *w)
{
    const struct graph *g = (const struct graph *)w;
    if (!g->automatic)
        return g->max > 0 ? g->max : 1;
    long peak = 0;
    for (int i = 0; i < g->count; i++)
        for (int s = 0; s < g->nseries; s++)
            if (g->values[i * GRAPH_SERIES + s] > peak)
                peak = g->values[i * GRAPH_SERIES + s];
    return graph_nice_max(peak, g->max);
}

static uint32_t line_color(const struct graph *g, const struct theme *t, int s)
{
    if (g->series[s].color)
        return g->series[s].color;
    return t->color[s == 0 ? TC_ACCENT : TC_TEXT];
}

static int heading_height(const struct painter *p)
{
    return painter_text_height(p) + 4;
}

/* The title on the left, and the value or the legend on the right. A
 * legend item is a colour square and the label of a series. Items that
 * do not fit beside the title are left out, from the first series on. */
static void paint_heading(struct graph *g, struct painter *p, int width)
{
    const struct theme *t = p->theme;
    painter_text(p, 0, 0, g->title, t->color[TC_TEXT]);
    int left = g->title[0] ? painter_text_width(p, g->title, -1) + 16 : 0;
    if (g->value[0]) {
        int x = width - painter_text_width(p, g->value, -1);
        if (x >= left)
            painter_text(p, x, 0, g->value, t->color[TC_TEXT_DISABLED]);
        return;
    }
    int th = painter_text_height(p), box = th / 2, right = width;
    for (int s = g->nseries - 1; s >= 0; s--) {
        if (!g->series[s].label[0])
            continue;
        int item = painter_text_width(p, g->series[s].label, -1) + (g->series[s].style != GRAPH_TEXT ? box + 5 : 0);
        if (right - item < left)
            break;
        right -= painter_text_width(p, g->series[s].label, -1);
        painter_text(p, right, 0, g->series[s].label, t->color[TC_TEXT_DISABLED]);
        if (g->series[s].style != GRAPH_TEXT) {
            right -= box + 5;
            painter_fill(p, right, (th - box) / 2, box, box, line_color(g, t, s));
        }
        right -= 16;
    }
}

/* One series in the plot rectangle. One sample covers w / (samples - 1)
 * pixels, the newest sample lies on the right edge. */
static void paint_series(struct graph *g, struct painter *p, int s, int x, int y, int w, int h, long max)
{
    const struct theme *t = p->theme;
    uint32_t line = line_color(g, t, s);
    uint32_t fill = g->series[s].style == GRAPH_AREA ? pixel_blend(t->color[TC_FIELD], line, 72) : 0;
    int span = g->samples > 1 ? g->samples - 1 : 1, px = 0, py = 0;
    for (int i = 0; i < g->count; i++) {
        int sx = x + w - 1 - (int)((long)(g->count - 1 - i) * (w - 1) / span);
        long v = g->values[i * GRAPH_SERIES + s];
        v = v > max ? max : v < 0 ? 0 : v;
        int sy = y + h - 1 - (int)(v * (h - 1) / max);
        if (i > 0) {
            if (fill)
                for (int cx = px; cx <= sx; cx++) {
                    int cy = sx == px ? sy : py + (sy - py) * (cx - px) / (sx - px);
                    painter_fill(p, cx, cy, 1, y + h - cy, fill);
                }
            painter_line(p, px, py, sx, sy, line);
        }
        px = sx;
        py = sy;
    }
}

static void graph_paint(struct widget *w, struct painter *p)
{
    struct graph *g = (struct graph *)w;
    const struct theme *t = p->theme;
    painter_fill(p, 0, 0, w->w, w->h, t->color[TC_WINDOW]);
    int th = heading_height(p);
    int hh = w->h > 3 * th && (g->title[0] || g->value[0] || g->nseries) ? th : 0;
    if (hh)
        paint_heading(g, p, w->w);
    int x = 0, y = hh, pw = w->w, ph = w->h - hh;
    if (pw < 4 || ph < 4)
        return;
    painter_fill(p, x, y, pw, ph, t->color[TC_FIELD]);
    uint32_t grid = pixel_blend(t->color[TC_FIELD], t->color[TC_BORDER], 80);
    for (int k = 1; k < 4; k++)
        painter_fill(p, x + 1, y + ph * k / 4, pw - 2, 1, grid);
    painter_frame(p, x, y, pw, ph, t->color[TC_BORDER]);
    long max = graph_scale(w);
    for (int s = 0; s < g->nseries; s++)
        if (g->series[s].style != GRAPH_TEXT)
            paint_series(g, p, s, x + 1, y + 1, pw - 2, ph - 2, max);
    if (g->format) {
        char text[32];
        g->format(max, text, sizeof text);
        painter_text(p, x + 4, y + 2, text, t->color[TC_TEXT_DISABLED]);
    }
}

static void graph_measure(struct widget *w, struct size_hint *h)
{
    const struct theme *t = widget_theme(w);
    h->pref_w = 160;
    h->pref_h = 100;
    h->min_w = 40;
    h->min_h = 2 * t->font->height;
}

static void graph_destroy(struct widget *w)
{
    free(((struct graph *)w)->values);
}

const struct widget_class graph_class = { "graph", sizeof(struct graph), graph_measure, NULL, graph_paint, NULL,
                                          graph_destroy };

struct widget *graph_new(struct widget *parent, int samples)
{
    struct widget *w = widget_new(&graph_class, parent);
    if (!w)
        return NULL;
    struct graph *g = (struct graph *)w;
    g->samples = samples > 1 ? samples : 2;
    g->values = calloc((size_t)g->samples * GRAPH_SERIES, sizeof *g->values);
    g->max = 1;
    if (!g->values) {
        widget_destroy(w);
        return NULL;
    }
    return w;
}

int graph_add_series(struct widget *w, const char *label, uint32_t color, enum graph_style style)
{
    struct graph *g = (struct graph *)w;
    if (g->nseries == GRAPH_SERIES)
        return -1;
    struct series *s = &g->series[g->nseries];
    snprintf(s->label, sizeof s->label, "%s", label ? label : "");
    s->color = color;
    s->style = style;
    widget_invalidate(w);
    return g->nseries++;
}

void graph_set_label(struct widget *w, int series, const char *label)
{
    struct graph *g = (struct graph *)w;
    if (series < 0 || series >= g->nseries || strcmp(g->series[series].label, label) == 0)
        return;
    snprintf(g->series[series].label, sizeof g->series[series].label, "%s", label);
    widget_invalidate(w);
}

void graph_set_style(struct widget *w, int series, enum graph_style style)
{
    struct graph *g = (struct graph *)w;
    if (series < 0 || series >= g->nseries || g->series[series].style == style)
        return;
    g->series[series].style = style;
    widget_invalidate(w);
}

void graph_push(struct widget *w, const long *values)
{
    struct graph *g = (struct graph *)w;
    if (g->count == g->samples) {
        memmove(g->values, g->values + GRAPH_SERIES, (size_t)(g->samples - 1) * GRAPH_SERIES * sizeof *g->values);
        g->count--;
    }
    for (int s = 0; s < GRAPH_SERIES; s++)
        g->values[g->count * GRAPH_SERIES + s] = s < g->nseries ? values[s] : 0;
    g->count++;
    widget_invalidate(w);
}

void graph_clear(struct widget *w)
{
    ((struct graph *)w)->count = 0;
    widget_invalidate(w);
}

void graph_set_scale(struct widget *w, long max, int automatic)
{
    struct graph *g = (struct graph *)w;
    g->max = max > 0 ? max : 1;
    g->automatic = automatic;
    widget_invalidate(w);
}

void graph_set_title(struct widget *w, const char *title, const char *value)
{
    struct graph *g = (struct graph *)w;
    snprintf(g->title, sizeof g->title, "%s", title ? title : "");
    snprintf(g->value, sizeof g->value, "%s", value ? value : "");
    widget_invalidate(w);
}

void graph_set_format(struct widget *w, void (*format)(long value, char *buf, size_t size))
{
    ((struct graph *)w)->format = format;
    widget_invalidate(w);
}
