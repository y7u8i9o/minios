/* Scroll bars and the scroll area (a viewport over one child). */
#include <gui/app.h>
#include <stdlib.h>

struct scrollbar {
    struct widget w;
    int vertical, page, grab;
};

int scroll_clamp(int v, int max, int page)
{
    int top = max - page;
    if (top < 0) top = 0;
    return v < 0 ? 0 : v > top ? top : v;
}

int scroll_set(int *value, int v, int max, int page)
{
    v = scroll_clamp(v, max, page);
    if (v == *value)
        return 0;
    *value = v;
    return 1;
}

/* The thumb lies 2 pixels inside both ends of the track. Its length is
 * the visible part of the range, at least 8 pixels at scale 100. */
int scrollbar_thumb(const struct theme *t, int len, int value, int max, int page, int *off, int *len_out)
{
    int track = len - 4;
    if (max <= 0 || page >= max || track <= 0) {
        *off = 0;
        *len_out = 0;
        return 0;
    }
    int th = track * page / max, min = theme_scale_px(t, 8);
    if (th < min) th = min;
    if (th > track) th = track;
    *off = 2 + (track - th) * scroll_clamp(value, max, page) / (max - page);
    *len_out = th;
    return track - th;
}

void scrollbar_paint_track(struct painter *p, int x, int y, int w, int h, int value, int max, int page, int vertical)
{
    const struct theme *t = p->theme;
    painter_fill(p, x, y, w, h, t->color[TC_TRACK]);
    int off, th;
    scrollbar_thumb(t, vertical ? h : w, value, max, page, &off, &th);
    if (!th)
        return;
    if (vertical)
        painter_rounded(p, x + 3, y + off, w - 6, th, t->color[TC_THUMB], 0xffffffffu);
    else
        painter_rounded(p, x + off, y + 3, th, h - 6, t->color[TC_THUMB], 0xffffffffu);
}

/* The value for a thumb whose start lies at pos along the track. */
static int value_at(const struct theme *t, int pos, int len, int max, int page)
{
    int off, th, span = scrollbar_thumb(t, len, 0, max, page, &off, &th);
    if (span <= 0)
        return 0;
    return scroll_clamp(((pos - 2) * (max - page) + span / 2) / span, max, page);
}

int scroll_track_event(struct scroll_track *t, struct widget *w, const struct event *e, struct rect r, int *value,
                       int max, int page)
{
    int pos = e->y - r.y;
    switch (e->type) {
    case EV_MOUSE_DOWN: {
        if (!(e->button & 1) || !rect_contains(r, e->x, e->y))
            return 0;
        int off, th;
        if (!scrollbar_thumb(widget_theme(w), r.h, *value, max, page, &off, &th))
            return 1;
        if (pos < off) {
            scroll_set(value, *value - page, max, page);
        } else if (pos >= off + th) {
            scroll_set(value, *value + page, max, page);
        } else {
            t->grab = pos - off;
            widget_capture(w);
        }
        return 1;
    }
    case EV_MOUSE_MOVE:
        if (t->grab < 0 || !(e->button & 1))
            return 0;
        scroll_set(value, value_at(widget_theme(w), pos - t->grab, r.h, max, page), max, page);
        return 1;
    case EV_MOUSE_UP:
        if (t->grab < 0)
            return 0;
        t->grab = -1;
        return 1;
    default:
        return 0;
    }
}

static void set_value(struct scrollbar *s, int v)
{
    if (!scroll_set(&s->w.value, v, s->w.max, s->page))
        return;
    widget_invalidate(&s->w);
    struct sig_scroll sc = { s->w.value };
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

static int scrollbar_event(struct widget *w, struct event *e)
{
    struct scrollbar *s = (struct scrollbar *)w;
    int pos = s->vertical ? e->y : e->x;
    switch (e->type) {
    case EV_MOUSE_DOWN: {
        if (!(e->button & 1))
            return 0;
        int off, th;
        if (!scrollbar_thumb(widget_theme(w), s->vertical ? w->h : w->w, w->value, w->max, s->page, &off, &th))
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
        set_value(s, value_at(widget_theme(w), pos - s->grab, s->vertical ? w->h : w->w, w->max, s->page));
        return 1;
    }
    case EV_MOUSE_WHEEL:
        set_value(s, w->value + 3 * e->button);
        return 1;
    case EV_KEY_DOWN:
        switch (e->code) {
        case KEY_UP: case KEY_LEFT: set_value(s, w->value - 1); return 1;
        case KEY_DOWN: case KEY_RIGHT: set_value(s, w->value + 1); return 1;
        case KEY_PAGEUP: set_value(s, w->value - s->page); return 1;
        case KEY_PAGEDOWN: set_value(s, w->value + s->page); return 1;
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
    max = max > 0 ? max : 0;
    page = page > 0 ? page : 1;
    value = scroll_clamp(value, max, page);
    if (w->max == max && s->page == page && w->value == value)
        return;
    w->max = max;
    s->page = page;
    w->value = value;
    widget_invalidate(w);
}

/* ---- scroll area ---- */

/* The content lies in a viewport inside the frame. The viewport clips
 * the content, so the content never covers the frame. */
struct scrollarea {
    struct widget w;
    struct widget *viewport, *content, *vbar, *hbar;
    int ox, oy;
};

static void viewport_measure(struct widget *w, struct size_hint *h)
{
    extern void widget_measure(struct widget *w);
    for (struct widget *c = w->first; c; c = c->next) {
        widget_measure(c);
        *h = c->measured;
    }
}

static const struct widget_class viewport_class = { "viewport", sizeof(struct widget), viewport_measure, NULL, NULL,
                                                    NULL, NULL };

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

/* A scroll moves the content inside the area. The size of the content
 * does not change, so the step needs no layout. A vertical step moves the
 * visible pixels by copy and repaints the exposed rows. A horizontal step
 * repaints the area. */
static int on_scroll(struct widget *bar, void *args, void *arg)
{
    struct scrollarea *a = arg;
    int dx = -a->hbar->value - a->content->x, dy = -a->vbar->value - a->content->y;
    a->content->x += dx;
    a->content->y += dy;
    if (dx || !dy) {
        widget_invalidate(&a->w);
        return 0;
    }
    int vw = a->w.w - 2 - (a->vbar->visible ? a->vbar->w : 0);
    int vh = a->w.h - 2 - (a->hbar->visible ? a->hbar->h : 0);
    widget_scroll_area(&a->w, (struct rect){ 1, 1, vw, vh }, dy);
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
    widget_show_in_layout(a->vbar, need_v);
    widget_show_in_layout(a->hbar, need_h);
    scrollbar_set(a->vbar, a->vbar->value, ch, vh);
    scrollbar_set(a->hbar, a->hbar->value, cw, vw);
    widget_set_rect(a->vbar, w->w - 1 - sb, 1, sb, vh);
    widget_set_rect(a->hbar, 1, w->h - 1 - sb, vw, sb);
    widget_set_rect(a->viewport, 1, 1, vw, vh);
    widget_set_rect(a->content, -a->hbar->value, -a->vbar->value, cw > vw ? cw : vw, ch > vh ? ch : vh);
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
    a->viewport = widget_new(&viewport_class, w);
    a->content = box_new(a->viewport, 1);
    a->vbar = scrollbar_new(w, 1);
    a->hbar = scrollbar_new(w, 0);
    a->vbar->focusable = a->hbar->focusable = 0;
    widget_connect(a->vbar, "scrolled", on_scroll, a);
    widget_connect(a->hbar, "scrolled", on_scroll, a);
    widget_set_stretch(w, 1, 1);
    w->user = a->content;               /* the content box for the application */
    return w;
}
