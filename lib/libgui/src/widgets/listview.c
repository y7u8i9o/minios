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
    int hot;                    /* the row under the pointer, or -1 */
};

static int line_h(const struct widget *w)
{
    const struct theme *t = widget_theme(w);
    return t->font->height + theme_px(t, TM_ROW_PAD);
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

/* The list scrolled from the first row old: the rows move by copy, and
 * the track is repainted. A move up exposes the rows at the bottom and
 * the partly shown row above them. */
static void scrolled(struct listview *l, int old)
{
    struct widget *w = &l->w;
    int lh = line_h(w), rows = rows_of(w);
    int sbw = l->nitems > rows ? theme_px(widget_theme(w), TM_SCROLLBAR) : 0;
    struct rect r = { 1, 1, w->w - 2 - sbw, w->h - 2 };
    int dy = (old - l->scroll) * lh, rem = r.h % lh;
    widget_scroll_area(w, r, dy);
    if (dy < 0 && rem)
        widget_invalidate_rect(w, (struct rect){ r.x, r.y + r.h + dy - rem, r.w, rem });
    widget_invalidate_rect(w, (struct rect){ w->w - sbw, 0, sbw, w->h });
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
    /* Only the rows inside the clip, for a partial paint. */
    struct rect clip = painter_clip_local(p);
    int i0 = clip.y > 0 ? clip.y / lh : 0, i1 = rect_empty(p->clip) ? 0 : (clip.y + clip.h + lh - 1) / lh;
    for (int i = i0; i < rows + 1 && i < i1 && l->scroll + i < l->nitems; i++) {
        int idx = l->scroll + i;
        int y = i * lh, selected = idx == w->value;
        /* Selected and hovered rows are rounded pills inside the list. */
        if (selected || (idx == l->hot && w->enabled))
            painter_rounded(p, 2, y + 1, w->w - 6 - sbw, lh - 2,
                            t->color[selected ? (w->enabled ? TC_SELECTION : TC_TRACK) : TC_BUTTON_HOVER], PAINTER_NONE);
        uint32_t fg = !w->enabled ? TC_TEXT_DISABLED : selected ? TC_SELECTION_TEXT : TC_TEXT;
        painter_text(p, 6, y + (lh - painter_text_height(p)) / 2, l->items[idx], t->color[fg]);
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
    int old_value = l->w.value, old_scroll = l->scroll;
    l->w.value = idx;
    ensure_visible(l);
    if (l->w.value != old_value || l->scroll != old_scroll)
        widget_invalidate(&l->w);
    struct sig_select s = { idx };
    widget_emit(&l->w, signal, &s);
}

/* Repaints the row idx where it is visible. */
static void invalidate_row(struct listview *l, int idx)
{
    int lh = line_h(&l->w), i = idx - l->scroll;
    if (idx >= 0 && i >= 0 && i <= rows_of(&l->w))
        widget_invalidate_rect(&l->w, (struct rect){ 1, 1 + i * lh, l->w.w - 2, lh });
}

static void set_hot(struct listview *l, int idx)
{
    if (idx == l->hot)
        return;
    invalidate_row(l, l->hot);
    l->hot = idx;
    invalidate_row(l, idx);
}

static int listview_event(struct widget *w, struct event *e)
{
    struct listview *l = (struct listview *)w;
    int lh = line_h(w), rows = rows_of(w);
    if (e->type == EV_MOUSE_MOVE) {
        int sbw = l->nitems > rows ? theme_px(widget_theme(w), TM_SCROLLBAR) : 0;
        int idx = l->scroll + (e->y - 1) / lh;
        set_hot(l, e->y >= 1 && e->x < w->w - sbw && idx < l->nitems ? idx : -1);
    } else if (e->type == EV_LEAVE) {
        set_hot(l, -1);
        return 1;
    }
    switch (e->type) {
    case EV_MOUSE_DOWN: case EV_MOUSE_MOVE: case EV_MOUSE_UP: {
        int sbw = l->nitems > rows ? theme_px(widget_theme(w), TM_SCROLLBAR) : 0;
        int before = l->scroll;
        struct rect track = { w->w - sbw, 0, sbw, w->h };
        if (sbw && scroll_track_event(&l->track, w, e, track, &l->scroll, l->nitems, rows)) {
            if (l->scroll != before)
                scrolled(l, before);
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
    case EV_MOUSE_WHEEL: {
        int old = l->scroll;
        if (scroll_set(&l->scroll, l->scroll + 3 * e->button, l->nitems, rows))
            scrolled(l, old);
        return 1;
    }
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
        ((struct listview *)w)->hot = -1;
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
