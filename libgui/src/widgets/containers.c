/* Tabs, split panes, tool bars and status bars. */
#include <gui/app.h>
#include <stdlib.h>
#include <string.h>

void widget_measure(struct widget *w);

/* ---- tabs ---- */

#define TAB_PAD 10

struct tabs {
    struct widget w;
    int autohide;               /* no title row while there is one page */
};

static int tabs_header_h(const struct widget *w)
{
    if (((const struct tabs *)w)->autohide && (!w->first || w->first == w->last))
        return 0;
    return theme_px(widget_theme(w), TM_CONTROL_H);
}

static void tabs_measure(struct widget *w, struct size_hint *h)
{
    int hh = tabs_header_h(w);
    int pw = 0, ph = 0;
    for (struct widget *c = w->first; c; c = c->next) {
        widget_measure(c);
        if (c->measured.pref_w > pw) pw = c->measured.pref_w;
        if (c->measured.pref_h > ph) ph = c->measured.pref_h;
    }
    h->pref_w = pw;
    h->pref_h = ph + hh + (hh ? theme_px(widget_theme(w), TM_SPACING) : 0);
    h->min_w = 40;
    h->min_h = hh + 20;
}

static void tabs_layout(struct widget *w)
{
    /* The page starts one spacing below the header row's hairline. */
    int hh = tabs_header_h(w), gap = hh ? theme_px(widget_theme(w), TM_SPACING) : 0;
    int i = 0;
    for (struct widget *c = w->first; c; c = c->next, i++) {
        c->visible = i == w->value;
        c->x = 0;
        c->y = hh + gap;
        c->w = w->w;
        c->h = w->h - hh - gap;
        c->dirty = 1;
    }
}

static int tab_x(struct widget *w, struct painter *p, int index, int *width)
{
    int x = 0, i = 0;
    for (struct widget *c = w->first; c; c = c->next, i++) {
        int tw = painter_text_width(p, widget_text(c), -1) + 2 * TAB_PAD;
        if (i == index) {
            *width = tw;
            return x;
        }
        x += tw;
    }
    *width = 0;
    return x;
}

static void tabs_paint(struct widget *w, struct painter *p)
{
    const struct theme *t = p->theme;
    int hh = tabs_header_h(w);
    if (!hh)
        return;
    /* Tab titles in a row over a hairline; the current one is marked
     * by an accent underline instead of a box. */
    painter_fill(p, 0, 0, w->w, hh, t->color[TC_WINDOW]);
    painter_fill(p, 0, hh - 1, w->w, 1, t->color[TC_BORDER]);
    int i = 0;
    for (struct widget *c = w->first; c; c = c->next, i++) {
        int tw;
        int x = tab_x(w, p, i, &tw);
        int current = i == w->value;
        if (current)
            painter_fill(p, x, hh - 3, tw, 3, t->color[TC_ACCENT]);
        painter_text(p, x + TAB_PAD, (hh - 3 - painter_text_height(p)) / 2, widget_text(c),
                     t->color[current ? TC_TEXT : TC_TEXT_DISABLED]);
    }
    if (w->focused) {
        int tw;
        int x = tab_x(w, p, w->value, &tw);
        painter_focus_ring(p, x + 2, 2, tw - 4, hh - 5);
    }
}

static int tabs_event(struct widget *w, struct event *e)
{
    if (e->type == EV_MOUSE_DOWN && (e->button & 1) && e->y < tabs_header_h(w)) {
        struct painter p;
        struct surface dummy = { NULL, 0, 0, 0 };
        painter_init(&p, &dummy, widget_theme(w));
        int i = 0;
        for (struct widget *c = w->first; c; c = c->next, i++) {
            int tw;
            int x = tab_x(w, &p, i, &tw);
            if (e->x >= x && e->x < x + tw) {
                tabs_select(w, i);
                break;
            }
        }
        return 1;
    }
    if (e->type == EV_KEY_DOWN) {
        if (e->code == KEY_RIGHT) { tabs_select(w, w->value + 1); return 1; }
        if (e->code == KEY_LEFT) { tabs_select(w, w->value - 1); return 1; }
    }
    if (e->type == EV_FOCUS_IN || e->type == EV_FOCUS_OUT) {
        widget_invalidate(w);
        return 1;
    }
    return 0;
}

const struct widget_class tabs_class = { "tabs", sizeof(struct tabs), tabs_measure, tabs_layout, tabs_paint, tabs_event, NULL };

struct widget *tabs_new(struct widget *parent)
{
    struct widget *w = widget_new(&tabs_class, parent);
    if (w) {
        w->focusable = 1;
        widget_set_stretch(w, 1, 1);
    }
    return w;
}

struct widget *tabs_add(struct widget *tabs, const char *title)
{
    struct widget *page = box_new(tabs, 1);
    if (page)
        widget_set_text(page, title);
    return page;
}

void tabs_set_autohide(struct widget *tabs, int on)
{
    ((struct tabs *)tabs)->autohide = on;
    widget_relayout(tabs);
}

void tabs_select(struct widget *tabs, int index)
{
    int n = 0;
    for (struct widget *c = tabs->first; c; c = c->next)
        n++;
    if (index < 0 || index >= n || index == tabs->value)
        return;
    tabs->value = index;
    widget_relayout(tabs);
    struct sig_select s = { index };
    widget_emit(tabs, "changed", &s);
}

/* ---- split pane ---- */

#define DIVIDER 6

struct split {
    struct widget w;
    int pos;                    /* size of the first child, -1 for half */
    int drag;
};

static void split_measure(struct widget *w, struct size_hint *h)
{
    int pw = 0, ph = 0;
    for (struct widget *c = w->first; c; c = c->next) {
        widget_measure(c);
        if (w->value) { ph += c->measured.pref_h; if (c->measured.pref_w > pw) pw = c->measured.pref_w; }
        else { pw += c->measured.pref_w; if (c->measured.pref_h > ph) ph = c->measured.pref_h; }
    }
    h->pref_w = pw + (w->value ? 0 : DIVIDER);
    h->pref_h = ph + (w->value ? DIVIDER : 0);
    h->min_w = h->min_h = 2 * DIVIDER;
}

static void split_layout(struct widget *w)
{
    struct split *s = (struct split *)w;
    struct widget *a = w->first, *b = a ? a->next : NULL;
    int total = (w->value ? w->h : w->w) - DIVIDER;
    int pos = s->pos < 0 ? total / 2 : s->pos;
    if (pos < 20) pos = 20;
    if (pos > total - 20) pos = total - 20;
    if (a) {
        a->x = a->y = 0;
        if (w->value) { a->w = w->w; a->h = pos; }
        else { a->w = pos; a->h = w->h; }
        a->dirty = 1;
    }
    if (b) {
        if (w->value) { b->x = 0; b->y = pos + DIVIDER; b->w = w->w; b->h = w->h - pos - DIVIDER; }
        else { b->x = pos + DIVIDER; b->y = 0; b->w = w->w - pos - DIVIDER; b->h = w->h; }
        b->dirty = 1;
    }
}

static void split_paint(struct widget *w, struct painter *p)
{
    painter_fill(p, 0, 0, w->w, w->h, p->theme->color[TC_WINDOW]);
    struct widget *a = w->first;
    if (!a)
        return;
    if (w->value)
        painter_fill(p, 0, a->h + 2, w->w, 2, p->theme->color[TC_TRACK]);
    else
        painter_fill(p, a->w + 2, 0, 2, w->h, p->theme->color[TC_TRACK]);
}

static int split_event(struct widget *w, struct event *e)
{
    struct split *s = (struct split *)w;
    struct widget *a = w->first;
    if (!a)
        return 0;
    int pos = w->value ? e->y : e->x;
    int start = w->value ? a->h : a->w;
    switch (e->type) {
    case EV_MOUSE_DOWN:
        if ((e->button & 1) && pos >= start && pos < start + DIVIDER) {
            s->drag = 1;
            widget_capture(w);
            return 1;
        }
        return 0;
    case EV_MOUSE_MOVE:
        if (s->drag) {
            splitpane_set_position(w, pos - DIVIDER / 2);
            return 1;
        }
        return 0;
    case EV_MOUSE_UP:
        s->drag = 0;
        return 1;
    default:
        return 0;
    }
}

const struct widget_class splitpane_class = { "splitpane", sizeof(struct split), split_measure, split_layout, split_paint, split_event, NULL };

struct widget *splitpane_new(struct widget *parent, int vertical)
{
    struct widget *w = widget_new(&splitpane_class, parent);
    if (w) {
        struct split *s = (struct split *)w;
        s->pos = -1;
        w->value = vertical;
        widget_set_stretch(w, 1, 1);
    }
    return w;
}

void splitpane_set_position(struct widget *w, int pos)
{
    ((struct split *)w)->pos = pos;
    widget_relayout(w);
}

/* ---- tool bar and status bar ---- */

static void bar_paint(struct widget *w, struct painter *p)
{
    painter_fill(p, 0, 0, w->w, w->h, p->theme->color[TC_WINDOW]);
    int y = w->cls == &toolbar_class ? w->h - 1 : 0;
    painter_line(p, 0, y, w->w - 1, y, p->theme->color[TC_BORDER]);
}

static void bar_measure(struct widget *w, struct size_hint *h) { box_class.measure(w, h); }
static void bar_layout(struct widget *w) { box_class.layout(w); }

const struct widget_class toolbar_class = { "toolbar", sizeof(struct widget), bar_measure, bar_layout, bar_paint, NULL, NULL };
const struct widget_class statusbar_class = { "statusbar", sizeof(struct widget), bar_measure, bar_layout, bar_paint, NULL, NULL };

struct widget *toolbar_new(struct widget *parent)
{
    struct widget *w = widget_new(&toolbar_class, parent);
    if (w) {
        w->value = 0;               /* horizontal box */
        w->padding = 2;
    }
    return w;
}

struct widget *toolbar_add(struct widget *toolbar, const char *icon, const char *tip)
{
    struct widget *b = button_new(toolbar, "");
    if (!b)
        return NULL;
    widget_set_icon(b, icon_get(icon));
    if (!b->icon)
        widget_set_text(b, tip);
    widget_set_tip(b, tip);
    return b;
}

struct widget *statusbar_new(struct widget *parent)
{
    struct widget *w = widget_new(&statusbar_class, parent);
    if (w) {
        w->value = 0;
        w->padding = 2;
    }
    return w;
}

struct widget *statusbar_add(struct widget *bar, int stretch)
{
    struct widget *l = label_new(bar, "");
    if (l)
        widget_set_stretch(l, stretch, 0);
    return l;
}
