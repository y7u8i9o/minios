/* Combo box, spinner, slider, progress bar. */
#include <gui/app.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../intl.h"

/* The width of the arrow part of combo boxes and spinners. */
static int arrow_w(const struct widget *w)
{
    return theme_scale_px(widget_theme(w), 18);
}

/* ---- combo box ---- */

struct combo {
    struct widget w;
    char **items;
    int nitems;
};

static void combo_measure(struct widget *w, struct size_hint *h)
{
    const struct theme *t = widget_theme(w);
    struct combo *c = (struct combo *)w;
    int tw = 60;
    for (int i = 0; i < c->nitems; i++) {
        int iw = widget_text_width(w, NULL, c->items[i], -1);
        if (iw > tw)
            tw = iw;
    }
    h->pref_w = tw + arrow_w(w) + 12;
    h->min_w = arrow_w(w) + 20;
    h->pref_h = h->min_h = theme_px(t, TM_CONTROL_H);
}

static void combo_paint(struct widget *w, struct painter *p)
{
    const struct theme *t = p->theme;
    painter_fill(p, 0, 0, w->w, w->h, t->color[TC_WINDOW]);
    uint32_t fill = !w->enabled ? t->color[TC_TRACK] : w->hover ? t->color[TC_BUTTON_HOVER] : t->color[TC_FIELD];
    painter_rounded(p, 0, 0, w->w, w->h, fill, t->color[w->focused ? TC_ACCENT : TC_BORDER]);
    painter_push(p, 1, 1, w->w - arrow_w(w) - 1, w->h - 2);
    painter_text(p, 4, (w->h - 2 - painter_text_height(p)) / 2, widget_text(w), t->color[w->enabled ? TC_TEXT : TC_TEXT_DISABLED]);
    painter_pop(p);
    int a = arrow_w(w);
    painter_chevron(p, w->w - a, (w->h - a) / 2, a, PAINTER_DOWN, t->color[w->enabled ? TC_TEXT : TC_TEXT_DISABLED]);
}

static int combo_pick(struct widget *list, void *args, void *arg)
{
    struct widget *w = arg;
    combobox_select(w, ((struct sig_select *)args)->index);
    window_popup_close(w->window);
    widget_focus(w);
    return 1;
}

static void combo_open(struct widget *w)
{
    struct combo *c = (struct combo *)w;
    const struct theme *t = widget_theme(w);
    struct widget *list = listview_new(NULL);
    for (int i = 0; i < c->nitems; i++)
        listview_add(list, c->items[i]);
    list->value = w->value;
    widget_connect(list, "selected", combo_pick, w);
    int ax, ay;
    widget_abs(w, &ax, &ay);
    int rows = c->nitems < 8 ? c->nitems : 8;
    if (rows < 1) rows = 1;
    window_popup_open(w->window, list, ax, ay + w->h, w->w, rows * (t->font->height + theme_px(t, TM_ROW_PAD)) + 2);
    widget_focus(list);
}

static int combo_event(struct widget *w, struct event *e)
{
    switch (e->type) {
    case EV_MOUSE_DOWN:
        if (e->button & 1)
            combo_open(w);
        return 1;
    case EV_MOUSE_WHEEL:
        combobox_select(w, w->value + (e->button > 0 ? 1 : -1));
        return 1;
    case EV_KEY_DOWN:
        if (e->code == KEY_DOWN) { combobox_select(w, w->value + 1); return 1; }
        if (e->code == KEY_UP) { combobox_select(w, w->value - 1); return 1; }
        if (e->ch == '\n' || e->ch == ' ') { combo_open(w); return 1; }
        return 0;
    case EV_FOCUS_IN: case EV_FOCUS_OUT: case EV_ENTER: case EV_LEAVE:
        widget_invalidate(w);
        return 1;
    default:
        return 0;
    }
}

static void combo_destroy(struct widget *w)
{
    combobox_clear(w);
}

const struct widget_class combobox_class = { "combobox", sizeof(struct combo), combo_measure, NULL, combo_paint, combo_event, combo_destroy };

struct widget *combobox_new(struct widget *parent)
{
    struct widget *w = widget_new(&combobox_class, parent);
    if (w) {
        w->focusable = 1;
        w->value = -1;
        widget_set_text(w, "");
    }
    return w;
}

void combobox_add(struct widget *w, const char *item)
{
    struct combo *c = (struct combo *)w;
    char **grown = realloc(c->items, (size_t)(c->nitems + 1) * sizeof *grown);
    if (!grown)
        return;
    c->items = grown;
    c->items[c->nitems++] = strdup(item);
    if (w->value < 0)
        combobox_select(w, 0);
    widget_relayout(w);
}

void combobox_clear(struct widget *w)
{
    struct combo *c = (struct combo *)w;
    for (int i = 0; i < c->nitems; i++)
        free(c->items[i]);
    free(c->items);
    c->items = NULL;
    c->nitems = 0;
    w->value = -1;
    widget_set_text(w, "");
}

const char *combobox_item(const struct widget *w, int index)
{
    const struct combo *c = (const struct combo *)w;
    return index >= 0 && index < c->nitems ? c->items[index] : NULL;
}

void combobox_select(struct widget *w, int index)
{
    struct combo *c = (struct combo *)w;
    if (index < 0 || index >= c->nitems || index == w->value)
        return;
    w->value = index;
    widget_set_text(w, c->items[index]);
    widget_invalidate(w);
    struct sig_select s = { index };
    widget_emit(w, "changed", &s);
}

/* ---- spinner ---- */

/* hot is the arrow under the pointer: 1 up, -1 down, 0 none. pressed is
 * the arrow pressed by the left button. */
struct spinner {
    struct widget w;
    int hot, pressed;
};

/* The arrow at a local position, or 0 outside the arrows. */
static int arrow_at(struct widget *w, int x, int y)
{
    if (x < w->w - arrow_w(w) || x >= w->w || y < 0 || y >= w->h)
        return 0;
    return y < w->h / 2 ? 1 : -1;
}

static void spinner_set(struct widget *w, int v)
{
    if (v < w->min) v = w->min;
    if (v > w->max) v = w->max;
    if (v == w->value)
        return;
    w->value = v;
    widget_invalidate(w);
    struct sig_change c = { v, NULL };
    widget_emit(w, "changed", &c);
}

static void spinner_measure(struct widget *w, struct size_hint *h)
{
    const struct theme *t = widget_theme(w);
    h->pref_w = 80;
    h->min_w = 50;
    h->pref_h = h->min_h = theme_px(t, TM_CONTROL_H);
}

static void spinner_paint(struct widget *w, struct painter *p)
{
    const struct theme *t = p->theme;
    painter_fill(p, 0, 0, w->w, w->h, t->color[TC_WINDOW]);
    struct spinner *sp = (struct spinner *)w;
    painter_rounded(p, 0, 0, w->w, w->h, t->color[w->enabled ? TC_FIELD : TC_TRACK],
                    t->color[w->focused ? TC_ACCENT : TC_BORDER]);
    char s[16];
    snprintf(s, sizeof s, "%d", w->value);
    painter_text(p, 4, (w->h - painter_text_height(p)) / 2, s, t->color[w->enabled ? TC_TEXT : TC_TEXT_DISABLED]);
    int a = arrow_w(w), ax = w->w - a, half = w->h / 2;
    for (int dir = 1; dir >= -1; dir -= 2) {
        int y = dir > 0 ? 1 : half;
        if (w->enabled && (sp->pressed == dir || sp->hot == dir))
            painter_fill(p, ax + 1, y, a - 2, half - 1,
                         t->color[sp->pressed == dir ? TC_BUTTON_PRESSED : TC_BUTTON_HOVER]);
        painter_chevron(p, ax + (a - half) / 2, y, half, dir > 0 ? PAINTER_UP : PAINTER_DOWN,
                        t->color[w->enabled ? TC_TEXT : TC_TEXT_DISABLED]);
    }
    painter_line(p, ax, 1, ax, w->h - 2, t->color[TC_BORDER]);
    painter_line(p, ax, half, w->w - 2, half, t->color[TC_BORDER]);
}

static int spinner_event(struct widget *w, struct event *e)
{
    struct spinner *sp = (struct spinner *)w;
    switch (e->type) {
    case EV_MOUSE_DOWN: {
        if (!(e->button & 1))
            return 0;
        int a = arrow_at(w, e->x, e->y);
        if (a) {
            sp->pressed = a;
            widget_capture(w);
            widget_invalidate(w);
            spinner_set(w, w->value + a);
        }
        return 1;
    }
    case EV_MOUSE_UP:
        if (!sp->pressed)
            return 0;
        sp->pressed = 0;
        widget_invalidate(w);
        return 1;
    case EV_MOUSE_MOVE: {
        int a = arrow_at(w, e->x, e->y);
        if (a != sp->hot) {
            sp->hot = a;
            widget_invalidate(w);
        }
        return 0;
    }
    case EV_LEAVE:
        if (sp->hot) {
            sp->hot = 0;
            widget_invalidate(w);
        }
        return 1;
    case EV_MOUSE_WHEEL:
        spinner_set(w, w->value - e->button);
        return 1;
    case EV_KEY_DOWN:
        if (e->code == KEY_UP) { spinner_set(w, w->value + 1); return 1; }
        if (e->code == KEY_DOWN) { spinner_set(w, w->value - 1); return 1; }
        if (e->code == KEY_PAGEUP) { spinner_set(w, w->value + 10); return 1; }
        if (e->code == KEY_PAGEDOWN) { spinner_set(w, w->value - 10); return 1; }
        return 0;
    case EV_FOCUS_IN: case EV_FOCUS_OUT:
        widget_invalidate(w);
        return 1;
    default:
        return 0;
    }
}

const struct widget_class spinner_class = { "spinner", sizeof(struct spinner), spinner_measure, NULL, spinner_paint, spinner_event, NULL };

struct widget *spinner_new(struct widget *parent, int min, int max, int value)
{
    struct widget *w = widget_new(&spinner_class, parent);
    if (w) {
        w->focusable = 1;
        w->min = min;
        w->max = max;
        w->value = value < min ? min : value > max ? max : value;
    }
    return w;
}

/* ---- slider ---- */

static int thumb_w(const struct widget *w)
{
    return theme_scale_px(widget_theme(w), 10);
}

static void slider_measure(struct widget *w, struct size_hint *h)
{
    h->pref_w = 120;
    h->min_w = 40;
    h->pref_h = h->min_h = theme_px(widget_theme(w), TM_CONTROL_H);
}

static int slider_thumb_x(const struct widget *w)
{
    int span = w->w - thumb_w(w);
    return w->max > w->min ? span * (w->value - w->min) / (w->max - w->min) : 0;
}

static void slider_paint(struct widget *w, struct painter *p)
{
    const struct theme *t = p->theme;
    painter_fill(p, 0, 0, w->w, w->h, t->color[TC_WINDOW]);
    int cy = w->h / 2;
    painter_fill(p, thumb_w(w) / 2, cy - 2, w->w - thumb_w(w), 4, t->color[TC_TRACK]);
    int tx = slider_thumb_x(w);
    painter_fill(p, thumb_w(w) / 2, cy - 2, tx, 4, t->color[w->enabled ? TC_ACCENT : TC_BORDER]);
    uint32_t thumb = !w->enabled ? t->color[TC_TRACK] : w->pressed || w->hover ? t->color[TC_ACCENT] : t->color[TC_THUMB];
    painter_rounded(p, tx, cy - 8, thumb_w(w), 16, thumb, w->enabled ? PAINTER_NONE : t->color[TC_BORDER]);
    if (w->focused)
        painter_focus_ring(p, 0, 0, w->w, w->h);
}

static void slider_set_from_x(struct widget *w, int x)
{
    int span = w->w - thumb_w(w);
    if (span <= 0)
        return;
    int pos = x - thumb_w(w) / 2;
    if (pos < 0) pos = 0;
    if (pos > span) pos = span;
    spinner_set(w, w->min + (pos * (w->max - w->min) + span / 2) / span);
}

static int slider_event(struct widget *w, struct event *e)
{
    switch (e->type) {
    case EV_MOUSE_DOWN:
        if (!(e->button & 1))
            return 0;
        w->pressed = 1;
        widget_capture(w);
        slider_set_from_x(w, e->x);
        widget_invalidate(w);
        return 1;
    case EV_MOUSE_MOVE:
        if (w->pressed)
            slider_set_from_x(w, e->x);
        return w->pressed;
    case EV_MOUSE_UP:
        if (!w->pressed)
            return 0;
        w->pressed = 0;
        widget_invalidate(w);
        return 1;
    case EV_MOUSE_WHEEL:
        spinner_set(w, w->value - e->button);
        return 1;
    case EV_KEY_DOWN:
        if (e->code == KEY_RIGHT || e->code == KEY_UP) { spinner_set(w, w->value + 1); return 1; }
        if (e->code == KEY_LEFT || e->code == KEY_DOWN) { spinner_set(w, w->value - 1); return 1; }
        if (e->code == KEY_HOME) { spinner_set(w, w->min); return 1; }
        if (e->code == KEY_END) { spinner_set(w, w->max); return 1; }
        return 0;
    case EV_FOCUS_IN: case EV_FOCUS_OUT: case EV_ENTER: case EV_LEAVE:
        widget_invalidate(w);
        return 1;
    default:
        return 0;
    }
}

const struct widget_class slider_class = { "slider", sizeof(struct widget), slider_measure, NULL, slider_paint, slider_event, NULL };

struct widget *slider_new(struct widget *parent, int min, int max, int value)
{
    struct widget *w = widget_new(&slider_class, parent);
    if (w) {
        w->focusable = 1;
        w->min = min;
        w->max = max;
        w->value = value < min ? min : value > max ? max : value;
        widget_set_stretch(w, 1, 0);
    }
    return w;
}

/* ---- progress bar ---- */

static void progress_measure(struct widget *w, struct size_hint *h)
{
    h->pref_w = 120;
    h->min_w = 20;
    /* The bar is as high as the text of its percentage. */
    h->pref_h = h->min_h = widget_theme(w)->font->height + 2;
}

static void progress_paint(struct widget *w, struct painter *p)
{
    const struct theme *t = p->theme;
    painter_rounded(p, 0, 0, w->w, w->h, t->color[TC_TRACK], 0xffffffffu);
    int span = w->max > w->min ? (w->w - 2) * (w->value - w->min) / (w->max - w->min) : 0;
    if (span > 0)
        painter_rounded(p, 1, 1, span, w->h - 2, t->color[w->enabled ? TC_ACCENT : TC_BORDER], PAINTER_NONE);
    char s[32];
    /* Some languages put a space or the percent sign before the number. */
    snprintf(s, sizeof s, _("%d%%"), w->max > w->min ? 100 * (w->value - w->min) / (w->max - w->min) : 0);
    int tw = painter_text_width(p, s, -1), th = painter_text_height(p);
    if (th <= w->h)
        painter_text(p, (w->w - tw) / 2, (w->h - th) / 2, s, t->color[TC_TEXT]);
}

const struct widget_class progress_class = { "progress", sizeof(struct widget), progress_measure, NULL, progress_paint, NULL, NULL };

struct widget *progress_new(struct widget *parent)
{
    struct widget *w = widget_new(&progress_class, parent);
    if (w) {
        w->max = 100;
        widget_set_stretch(w, 1, 0);
    }
    return w;
}
