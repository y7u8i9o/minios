/* List of strings with selection, keyboard navigation, wheel and an
 * internal scroll bar. */
#include <gui/app.h>
#include <stdlib.h>
#include <string.h>

struct listview {
    struct widget w;
    char **items;
    int nitems, scroll;
    struct scroll_track track;
    struct gui_clicks clicks;
};

static int line_h(const struct widget *w)
{
    return widget_theme(w)->font->height + 4;
}

static int rows_of(const struct widget *w)
{
    int lh = line_h(w);
    return lh ? (w->h - 2) / lh : 0;
}

static void listview_measure(struct widget *w, struct size_hint *h)
{
    h->pref_w = 160;
    h->pref_h = 4 * line_h(w) + 2;
    h->min_w = 40;
    h->min_h = line_h(w) + 2;
}

static void listview_paint(struct widget *w, struct painter *p)
{
    struct listview *l = (struct listview *)w;
    const struct theme *t = p->theme;
    int lh = line_h(w), rows = rows_of(w);
    int bar = l->nitems > rows;
    int sbw = bar ? theme_px(t, TM_SCROLLBAR) : 0;
    painter_fill(p, 0, 0, w->w, w->h, t->color[TC_FIELD]);
    painter_frame(p, 0, 0, w->w, w->h, t->color[w->focused ? TC_ACCENT : TC_BORDER]);
    painter_push(p, 1, 1, w->w - 2 - sbw, w->h - 2);
    for (int i = 0; i < rows + 1 && l->scroll + i < l->nitems; i++) {
        int idx = l->scroll + i;
        int y = i * lh;
        if (idx == w->value)
            painter_fill(p, 0, y, w->w, lh, t->color[TC_SELECTION]);
        painter_text(p, 3, y + 2, l->items[idx], idx == w->value ? t->color[TC_SELECTION_TEXT] : t->color[TC_TEXT]);
    }
    painter_pop(p);
    if (bar)
        scrollbar_paint_track(p, w->w - sbw, 0, sbw, w->h, l->scroll, l->nitems, rows, 1);
}

static void ensure_visible(struct listview *l)
{
    int rows = rows_of(&l->w);
    if (rows <= 0)
        return;
    if (l->w.value < l->scroll)
        l->scroll = l->w.value;
    if (l->w.value >= l->scroll + rows)
        l->scroll = l->w.value - rows + 1;
}

static void select_row(struct listview *l, int idx, const char *signal)
{
    if (idx < 0 || idx >= l->nitems)
        return;
    l->w.value = idx;
    ensure_visible(l);
    widget_invalidate(&l->w);
    struct sig_select s = { idx };
    widget_emit(&l->w, signal, &s);
}

static int listview_event(struct widget *w, struct event *e)
{
    struct listview *l = (struct listview *)w;
    int lh = line_h(w), rows = rows_of(w);
    switch (e->type) {
    case EV_MOUSE_DOWN: case EV_MOUSE_MOVE: case EV_MOUSE_UP: {
        int sbw = l->nitems > rows ? theme_px(widget_theme(w), TM_SCROLLBAR) : 0;
        int before = l->scroll;
        struct rect track = { w->w - sbw, 0, sbw, w->h };
        if (sbw && scroll_track_event(&l->track, w, e, track, &l->scroll, l->nitems, rows)) {
            if (l->scroll != before)
                widget_invalidate(w);
            return 1;
        }
        if (e->type != EV_MOUSE_DOWN || !(e->button & 1))
            return 0;
        /* A second click on the selected row activates it. */
        int idx = l->scroll + (e->y - 1) / lh;
        int count = gui_click_count(&l->clicks, e->x, e->y);
        if (idx >= 0 && idx < l->nitems)
            select_row(l, idx, count == 2 && idx == w->value ? "activate" : "selected");
        return 1;
    }
    case EV_MOUSE_WHEEL:
        l->scroll = scroll_clamp(l->scroll + 3 * e->button, l->nitems, rows);
        widget_invalidate(w);
        return 1;
    case EV_KEY_DOWN:
        switch (e->code) {
        case KEY_UP: select_row(l, w->value - 1, "selected"); return 1;
        case KEY_DOWN: select_row(l, w->value + 1, "selected"); return 1;
        case KEY_PAGEUP: select_row(l, w->value - rows < 0 ? 0 : w->value - rows, "selected"); return 1;
        case KEY_PAGEDOWN: select_row(l, w->value + rows >= l->nitems ? l->nitems - 1 : w->value + rows, "selected"); return 1;
        case KEY_HOME: select_row(l, 0, "selected"); return 1;
        case KEY_END: select_row(l, l->nitems - 1, "selected"); return 1;
        }
        if (e->ch == '\n' && w->value >= 0) {
            select_row(l, w->value, "activate");
            return 1;
        }
        return 0;
    case EV_FOCUS_IN: case EV_FOCUS_OUT:
        widget_invalidate(w);
        return 1;
    default:
        return 0;
    }
}

static void listview_destroy(struct widget *w)
{
    listview_clear(w);
}

const struct widget_class listview_class = { "listview", sizeof(struct listview), listview_measure, NULL, listview_paint, listview_event, listview_destroy };

struct widget *listview_new(struct widget *parent)
{
    struct widget *w = widget_new(&listview_class, parent);
    if (w) {
        w->focusable = 1;
        w->value = -1;
        ((struct listview *)w)->track.grab = -1;
        widget_set_stretch(w, 1, 1);
    }
    return w;
}

void listview_clear(struct widget *w)
{
    struct listview *l = (struct listview *)w;
    for (int i = 0; i < l->nitems; i++)
        free(l->items[i]);
    free(l->items);
    l->items = NULL;
    l->nitems = 0;
    l->scroll = 0;
    w->value = -1;
    widget_invalidate(w);
}

void listview_add(struct widget *w, const char *item)
{
    struct listview *l = (struct listview *)w;
    char **grown = realloc(l->items, (size_t)(l->nitems + 1) * sizeof *grown);
    if (!grown)
        return;
    l->items = grown;
    l->items[l->nitems++] = strdup(item);
    widget_invalidate(w);
}

int listview_count(const struct widget *w)
{
    return ((const struct listview *)w)->nitems;
}

const char *listview_item(const struct widget *w, int index)
{
    const struct listview *l = (const struct listview *)w;
    return index >= 0 && index < l->nitems ? l->items[index] : NULL;
}
