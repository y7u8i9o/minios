/* Combo box, spinner, slider, progress bar. */
#include <gui/app.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ARROW_W 18

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
        int iw = gfx_text_width_font(t->font, c->items[i], -1);
        if (iw > tw)
            tw = iw;
    }
    h->pref_w = tw + ARROW_W + 12;
    h->min_w = ARROW_W + 20;
    h->pref_h = h->min_h = theme_px(t, TM_CONTROL_H);
}

static void combo_paint(struct widget *w, struct painter *p)
{
    const struct theme *t = p->theme;
    painter_fill(p, 0, 0, w->w, w->h, t->color[TC_WINDOW]);
    painter_rounded(p, 0, 0, w->w, w->h, t->color[TC_FIELD], t->color[w->focused ? TC_ACCENT : TC_BORDER]);
    painter_push(p, 1, 1, w->w - ARROW_W - 1, w->h - 2);
    painter_text(p, 4, (w->h - 2 - painter_text_height(p)) / 2, widget_text(w), t->color[w->enabled ? TC_TEXT : TC_TEXT_DISABLED]);
    painter_pop(p);
    int ax = w->w - ARROW_W + 4, ay = w->h / 2 - 2;
    for (int i = 0; i < 5; i++)
        painter_line(p, ax + i, ay + i, ax + 9 - i, ay + i, t->color[TC_TEXT]);
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
    window_popup_open(w->window, list, ax, ay + w->h, w->w, rows * (t->font->height + 4) + 2);
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
    case EV_FOCUS_IN: case EV_FOCUS_OUT:
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
    painter_rounded(p, 0, 0, w->w, w->h, t->color[TC_FIELD], t->color[w->focused ? TC_ACCENT : TC_BORDER]);
    char s[16];
    snprintf(s, sizeof s, "%d", w->value);
    painter_text(p, 4, (w->h - painter_text_height(p)) / 2, s, t->color[w->enabled ? TC_TEXT : TC_TEXT_DISABLED]);
    int ax = w->w - ARROW_W;
    painter_line(p, ax, 1, ax, w->h - 2, t->color[TC_BORDER]);
    painter_line(p, ax, w->h / 2, w->w - 2, w->h / 2, t->color[TC_BORDER]);
    for (int i = 0; i < 4; i++) {
        painter_line(p, ax + 5 + i, w->h / 2 - 3 - i, ax + 12 - i, w->h / 2 - 3 - i, t->color[TC_TEXT]);
        painter_line(p, ax + 5 + i, w->h / 2 + 3 + i, ax + 12 - i, w->h / 2 + 3 + i, t->color[TC_TEXT]);
    }
}

static int spinner_event(struct widget *w, struct event *e)
{
    switch (e->type) {
    case EV_MOUSE_DOWN:
        if (!(e->button & 1))
            return 0;
        if (e->x >= w->w - ARROW_W)
            spinner_set(w, w->value + (e->y < w->h / 2 ? 1 : -1));
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

const struct widget_class spinner_class = { "spinner", sizeof(struct widget), spinner_measure, NULL, spinner_paint, spinner_event, NULL };

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

#define THUMB_W 10

static void slider_measure(struct widget *w, struct size_hint *h)
{
    h->pref_w = 120;
    h->min_w = 40;
    h->pref_h = h->min_h = theme_px(widget_theme(w), TM_CONTROL_H);
}

static int slider_thumb_x(const struct widget *w)
{
    int span = w->w - THUMB_W;
    return w->max > w->min ? span * (w->value - w->min) / (w->max - w->min) : 0;
}

static void slider_paint(struct widget *w, struct painter *p)
{
    const struct theme *t = p->theme;
    painter_fill(p, 0, 0, w->w, w->h, t->color[TC_WINDOW]);
    int cy = w->h / 2;
    painter_fill(p, THUMB_W / 2, cy - 2, w->w - THUMB_W, 4, t->color[TC_TRACK]);
    int tx = slider_thumb_x(w);
    painter_fill(p, THUMB_W / 2, cy - 2, tx, 4, t->color[TC_ACCENT]);
    painter_rounded(p, tx, cy - 8, THUMB_W, 16, w->pressed ? t->color[TC_BUTTON_PRESSED] : t->color[TC_THUMB], 0xffffffffu);
    if (w->focused)
        painter_focus_ring(p, 0, 0, w->w, w->h);
}

static void slider_set_from_x(struct widget *w, int x)
{
    int span = w->w - THUMB_W;
    if (span <= 0)
        return;
    int pos = x - THUMB_W / 2;
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
    case EV_FOCUS_IN: case EV_FOCUS_OUT:
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
    h->pref_h = h->min_h = 14;
}

static void progress_paint(struct widget *w, struct painter *p)
{
    const struct theme *t = p->theme;
    painter_rounded(p, 0, 0, w->w, w->h, t->color[TC_TRACK], 0xffffffffu);
    int span = w->max > w->min ? (w->w - 2) * (w->value - w->min) / (w->max - w->min) : 0;
    if (span > 0)
        painter_fill(p, 1, 1, span, w->h - 2, t->color[TC_ACCENT]);
    char s[16];
    snprintf(s, sizeof s, "%d%%", w->max > w->min ? 100 * (w->value - w->min) / (w->max - w->min) : 0);
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
