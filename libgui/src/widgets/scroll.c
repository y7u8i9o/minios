/* Scroll bars and the scroll area (a viewport over one child). */
#include <gui/app.h>
#include <stdlib.h>

struct scrollbar {
    struct widget w;
    int vertical, page, grab;
};

void scrollbar_paint_track(struct painter *p, int x, int y, int w, int h, int value, int max, int page, int vertical);
void scrollbar_paint_track(struct painter *p, int x, int y, int w, int h, int value, int max, int page, int vertical)
{
    const struct theme *t = p->theme;
    painter_fill(p, x, y, w, h, t->color[TC_TRACK]);
    if (max <= 0 || page >= max)
        return;
    int track = (vertical ? h : w) - 4;
    int th = track * page / max;
    if (th < 8) th = 8;
    int off = 2 + (track - th) * value / (max - page);
    if (vertical)
        painter_rounded(p, x + 3, y + off, w - 6, th, t->color[TC_THUMB], 0xffffffffu);
    else
        painter_rounded(p, x + off, y + 3, th, h - 6, t->color[TC_THUMB], 0xffffffffu);
}

static int clamp(struct scrollbar *s, int v)
{
    int top = s->w.max - s->page;
    if (top < 0) top = 0;
    return v < 0 ? 0 : v > top ? top : v;
}

static void set_value(struct scrollbar *s, int v)
{
    v = clamp(s, v);
    if (v == s->w.value)
        return;
    s->w.value = v;
    widget_invalidate(&s->w);
    struct sig_scroll sc = { v };
    widget_emit(&s->w, "scrolled", &sc);
}

static void scrollbar_measure(struct widget *w, struct size_hint *h)
{
    struct scrollbar *s = (struct scrollbar *)w;
    int sb = theme_px(widget_theme(w), TM_SCROLLBAR);
    if (s->vertical) {
        h->pref_w = h->min_w = h->max_w = sb;
        h->pref_h = 60;
        h->min_h = 20;
    } else {
        h->pref_h = h->min_h = h->max_h = sb;
        h->pref_w = 60;
        h->min_w = 20;
    }
}

static void scrollbar_paint(struct widget *w, struct painter *p)
{
    struct scrollbar *s = (struct scrollbar *)w;
    scrollbar_paint_track(p, 0, 0, w->w, w->h, w->value, w->max, s->page, s->vertical);
    if (w->focused)
        painter_focus_ring(p, 0, 0, w->w, w->h);
}

static int thumb_geometry(struct scrollbar *s, int *off, int *th)
{
    int track = (s->vertical ? s->w.h : s->w.w) - 2;
    if (s->w.max <= 0 || s->page >= s->w.max)
        return 0;
    *th = track * s->page / s->w.max;
    if (*th < 8) *th = 8;
    *off = 1 + (track - *th) * s->w.value / (s->w.max - s->page);
    return track - *th;
}

static int scrollbar_event(struct widget *w, struct event *e)
{
    struct scrollbar *s = (struct scrollbar *)w;
    int pos = s->vertical ? e->y : e->x;
    switch (e->type) {
    case EV_MOUSE_DOWN: {
        if (!(e->button & 1))
            return 0;
        int off, th;
        if (!thumb_geometry(s, &off, &th))
            return 1;
        if (pos < off)
            set_value(s, w->value - s->page);
        else if (pos >= off + th)
            set_value(s, w->value + s->page);
        else {
            s->grab = pos - off;
            widget_capture(w);
        }
        return 1;
    }
    case EV_MOUSE_MOVE: {
        if (!(e->button & 1) || !w->window || window_state_of(w->window)->capture != w)
            return 0;
        int off, th;
        int span = thumb_geometry(s, &off, &th);
        if (span > 0)
            set_value(s, (pos - s->grab - 1) * (w->max - s->page) / span);
        return 1;
    }
    case EV_MOUSE_WHEEL:
        set_value(s, w->value + 3 * e->button);
        return 1;
    case EV_KEY_DOWN:
        switch (e->code) {
        case 0xc8: case 0xcb: set_value(s, w->value - 1); return 1;
        case 0xd0: case 0xcd: set_value(s, w->value + 1); return 1;
        case 0xc9: set_value(s, w->value - s->page); return 1;
        case 0xd1: set_value(s, w->value + s->page); return 1;
        }
        return 0;
    default:
        return 0;
    }
}

const struct widget_class scrollbar_class = { "scrollbar", sizeof(struct scrollbar), scrollbar_measure, NULL, scrollbar_paint, scrollbar_event, NULL };

struct widget *scrollbar_new(struct widget *parent, int vertical)
{
    struct widget *w = widget_new(&scrollbar_class, parent);
    if (!w)
        return NULL;
    struct scrollbar *s = (struct scrollbar *)w;
    s->vertical = vertical;
    s->page = 1;
    w->max = 1;
    w->focusable = 1;
    widget_set_stretch(w, vertical ? 0 : 1, vertical ? 1 : 0);
    return w;
}

void scrollbar_set(struct widget *w, int value, int max, int page)
{
    struct scrollbar *s = (struct scrollbar *)w;
    w->max = max > 0 ? max : 0;
    s->page = page > 0 ? page : 1;
    w->value = clamp(s, value);
    widget_invalidate(w);
}

/* ---- scroll area ---- */

struct scrollarea {
    struct widget w;
    struct widget *content, *vbar, *hbar;
    int ox, oy;
};

static void scrollarea_measure(struct widget *w, struct size_hint *h)
{
    extern void widget_measure(struct widget *w);
    struct scrollarea *a = (struct scrollarea *)w;
    for (struct widget *c = w->first; c; c = c->next)
        widget_measure(c);
    h->pref_w = a->content ? a->content->measured.pref_w / 2 + 40 : 100;
    h->pref_h = a->content ? a->content->measured.pref_h / 2 + 40 : 100;
    h->min_w = h->min_h = 40;
}

static int on_scroll(struct widget *bar, void *args, void *arg)
{
    struct scrollarea *a = arg;
    widget_invalidate(&a->w);
    widget_relayout(&a->w);
    return 0;
}

static void scrollarea_layout(struct widget *w)
{
    struct scrollarea *a = (struct scrollarea *)w;
    if (!a->content)
        return;
    int sb = theme_px(widget_theme(w), TM_SCROLLBAR);
    int cw = a->content->measured.pref_w, ch = a->content->measured.pref_h;
    int need_v = ch > w->h - 2, need_h = cw > w->w - 2;
    if (need_v && cw > w->w - 2 - sb) need_h = 1;
    if (need_h && ch > w->h - 2 - sb) need_v = 1;
    int vw = w->w - 2 - (need_v ? sb : 0), vh = w->h - 2 - (need_h ? sb : 0);
    widget_set_visible(a->vbar, need_v);
    widget_set_visible(a->hbar, need_h);
    scrollbar_set(a->vbar, a->vbar->value, ch, vh);
    scrollbar_set(a->hbar, a->hbar->value, cw, vw);
    a->vbar->x = w->w - 1 - sb; a->vbar->y = 1; a->vbar->w = sb; a->vbar->h = vh;
    a->hbar->x = 1; a->hbar->y = w->h - 1 - sb; a->hbar->w = vw; a->hbar->h = sb;
    a->content->x = 1 - a->hbar->value;
    a->content->y = 1 - a->vbar->value;
    a->content->w = cw > vw ? cw : vw;
    a->content->h = ch > vh ? ch : vh;
    a->content->dirty = 1;
    a->vbar->dirty = a->hbar->dirty = 1;
    w->needs_layout = 0;
}

static void scrollarea_paint(struct widget *w, struct painter *p)
{
    painter_fill(p, 0, 0, w->w, w->h, p->theme->color[TC_FIELD]);
    painter_frame(p, 0, 0, w->w, w->h, p->theme->color[TC_BORDER]);
}

static int scrollarea_event(struct widget *w, struct event *e)
{
    struct scrollarea *a = (struct scrollarea *)w;
    if (e->type == EV_MOUSE_WHEEL && a->vbar->visible) {
        struct event f = *e;
        return scrollbar_class.event(a->vbar, &f);
    }
    return 0;
}

const struct widget_class scrollarea_class = { "scrollarea", sizeof(struct scrollarea), scrollarea_measure, scrollarea_layout, scrollarea_paint, scrollarea_event, NULL };

struct widget *scrollarea_new(struct widget *parent)
{
    struct widget *w = widget_new(&scrollarea_class, parent);
    if (!w)
        return NULL;
    struct scrollarea *a = (struct scrollarea *)w;
    a->content = box_new(w, 1);
    a->vbar = scrollbar_new(w, 1);
    a->hbar = scrollbar_new(w, 0);
    a->vbar->focusable = a->hbar->focusable = 0;
    widget_connect(a->vbar, "scrolled", on_scroll, a);
    widget_connect(a->hbar, "scrolled", on_scroll, a);
    widget_set_stretch(w, 1, 1);
    w->user = a->content;               /* the content box for the application */
    return w;
}
