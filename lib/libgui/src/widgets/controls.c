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

/* An editable combo box has a text field as its child, left of the
 * arrow. */
struct combo {
    struct widget w;
    char **items;
    int nitems;
    struct widget *field;
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
    if (((struct combo *)w)->field) {
        /* The field paints the text. The arrow is a button beside it. */
        int a = arrow_w(w) + 2;
        painter_rounded(p, w->w - a, 0, a, w->h, w->hover && w->enabled ? t->color[TC_BUTTON_HOVER] : t->color[TC_BUTTON],
                        t->color[TC_BORDER]);
        painter_chevron(p, w->w - a + 1, (w->h - a + 2) / 2, a - 2, PAINTER_DOWN,
                        t->color[w->enabled ? TC_TEXT : TC_TEXT_DISABLED]);
        return;
    }
    painter_field(p, 0, 0, w->w, w->h, widget_paint_state(w) & ~PAINTER_PRESSED);
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
        if ((e->button & 1) && (!((struct combo *)w)->field || e->x >= w->w - arrow_w(w) - 2))
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

static void combo_layout(struct widget *w)
{
    struct widget *field = ((struct combo *)w)->field;
    if (field)
        widget_set_rect(field, 0, 0, w->w - arrow_w(w) - 4, w->h);
}

const struct widget_class combobox_class = { "combobox", sizeof(struct combo), combo_measure, combo_layout, combo_paint,
                                             combo_event, combo_destroy };

/* The text of an editable combo box follows its field. A typed text
 * selects no item. */
static int combo_field_changed(struct widget *field, void *args, void *arg)
{
    struct widget *w = arg;
    free(w->text);
    w->text = strdup(widget_text(field));
    w->value = -1;
    struct sig_select s = { -1 };
    widget_emit(w, "changed", &s);
    return 1;
}

static int combo_field_activate(struct widget *field, void *args, void *arg)
{
    struct sig_change c = { 0, widget_text(field) };
    widget_emit(arg, "activate", &c);
    return 1;
}

void combobox_set_editable(struct widget *w, int editable)
{
    struct combo *c = (struct combo *)w;
    if (!editable == !c->field)
        return;
    if (editable) {
        c->field = textfield_new(w, widget_text(w));
        widget_connect(c->field, "changed", combo_field_changed, w);
        widget_connect(c->field, "activate", combo_field_activate, w);
        w->focusable = 0;
    } else {
        widget_destroy(c->field);
        c->field = NULL;
        w->focusable = 1;
    }
    widget_relayout(w);
}

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
    if (c->field)
        widget_set_text(c->field, c->items[index]);
    widget_invalidate(w);
    struct sig_select s = { index };
    widget_emit(w, "changed", &s);
}

/* ---- spinner ---- */

/* hot is the arrow under the pointer: 1 up, -1 down, 0 none. pressed is
 * the arrow pressed by the left button. edit is the number that the user
 * types, shown while editing is set. */
struct spinner {
    struct widget w;
    int hot, pressed;
    char edit[12];
    int editing;
};

static void spinner_set(struct widget *w, int v);

/* Enter or a change of the focus commits a typed number, clamped to the
 * range. An empty number leaves the value. */
static void spinner_commit(struct widget *w)
{
    struct spinner *sp = (struct spinner *)w;
    if (!sp->editing)
        return;
    sp->editing = 0;
    widget_invalidate(w);
    if (sp->edit[0] && strcmp(sp->edit, "-") != 0)
        spinner_set(w, atoi(sp->edit));
}

/* Typed digits and a leading minus sign edit the number. */
static int spinner_type(struct widget *w, int ch)
{
    struct spinner *sp = (struct spinner *)w;
    size_t n = sp->editing ? strlen(sp->edit) : 0;
    if (ch == '\b') {
        if (!sp->editing)
            snprintf(sp->edit, sizeof sp->edit, "%d", w->value), n = strlen(sp->edit);
        if (n)
            sp->edit[n - 1] = '\0';
    } else if ((ch >= '0' && ch <= '9') || (ch == '-' && n == 0 && w->min < 0)) {
        if (n + 1 >= sizeof sp->edit)
            return 1;
        sp->edit[n] = (char)ch;
        sp->edit[n + 1] = '\0';
    } else {
        return 0;
    }
    sp->editing = 1;
    widget_invalidate(w);
    return 1;
}

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
    painter_field(p, 0, 0, w->w, w->h, widget_paint_state(w) & (PAINTER_DISABLED | PAINTER_FOCUSED));
    char s[16];
    snprintf(s, sizeof s, "%d", w->value);
    const char *shown_text = sp->editing ? sp->edit : s;
    int ty = (w->h - painter_text_height(p)) / 2;
    painter_text(p, 4, ty, shown_text, t->color[w->enabled ? TC_TEXT : TC_TEXT_DISABLED]);
    if (sp->editing && w->focused) {
        int cx = 4 + painter_text_width(p, shown_text, -1);
        painter_line(p, cx, ty, cx, ty + painter_text_height(p) - 1, t->color[TC_TEXT]);
    }
    int a = arrow_w(w), ax = w->w - a, half = w->h / 2;
    for (int dir = 1; dir >= -1; dir -= 2) {
        int y = dir > 0 ? 1 : half;
        if (w->enabled)
            painter_button(p, ax + 1, y, a - 2, half - 1,
                           PAINTER_FLAT | (sp->pressed == dir ? PAINTER_PRESSED : sp->hot == dir ? PAINTER_HOVER : 0));
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
        if (e->mods & (WMOD_CTRL | WMOD_ALT))
            return 0;
        if (e->ch == '\n') {
            int was = sp->editing;
            spinner_commit(w);
            return was;
        }
        if (e->code == KEY_ESC && sp->editing) {
            sp->editing = 0;
            widget_invalidate(w);
            return 1;
        }
        if (e->code == KEY_UP || e->code == KEY_DOWN || e->code == KEY_PAGEUP || e->code == KEY_PAGEDOWN)
            spinner_commit(w);
        if (e->code == KEY_UP) { spinner_set(w, w->value + 1); return 1; }
        if (e->code == KEY_DOWN) { spinner_set(w, w->value - 1); return 1; }
        if (e->code == KEY_PAGEUP) { spinner_set(w, w->value + 10); return 1; }
        if (e->code == KEY_PAGEDOWN) { spinner_set(w, w->value - 10); return 1; }
        return spinner_type(w, e->ch);
    case EV_FOCUS_OUT:
        spinner_commit(w);
        widget_invalidate(w);
        return 1;
    case EV_FOCUS_IN:
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
    /* The track runs between the centres of the thumb at both ends. */
    int tw = thumb_w(w), track = theme_scale_px(t, 4);
    painter_slider(p, tw / 2, (w->h - track) / 2, w->w - tw, track, slider_thumb_x(w), tw, theme_scale_px(t, 16),
                   widget_paint_state(w));
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

/* A pulsing bar moves a block back and forth instead of showing a value.
 * The timer advances phase every PULSE_MS. */
#define PULSE_MS 50
#define PULSE_STEPS 40

struct progress {
    struct widget w;
    int pulse, phase;
    struct timer *timer;
};

static void pulse_step(void *arg)
{
    struct progress *pr = arg;
    pr->phase = (pr->phase + 1) % (2 * PULSE_STEPS);
    widget_invalidate(&pr->w);
}

static void progress_paint(struct widget *w, struct painter *p)
{
    const struct theme *t = p->theme;
    struct progress *pr = (struct progress *)w;
    uint32_t bar = t->color[w->enabled ? TC_ACCENT : TC_BORDER];
    if (pr->pulse) {
        if (!pr->timer && w->app)
            pr->timer = app_timer_add(w->app, PULSE_MS, 1, pulse_step, pr);
        int bw = w->w / 4, pos = pr->phase < PULSE_STEPS ? pr->phase : 2 * PULSE_STEPS - pr->phase;
        painter_meter(p, 0, 0, w->w, w->h, (w->w - bw) * pos / PULSE_STEPS, bw, bar);
        return;
    }
    int span = w->max > w->min ? w->w * (w->value - w->min) / (w->max - w->min) : 0;
    painter_meter(p, 0, 0, w->w, w->h, 0, span, bar);
    char s[32];
    /* Some languages put a space or the percent sign before the number. */
    snprintf(s, sizeof s, _("%d%%"), w->max > w->min ? 100 * (w->value - w->min) / (w->max - w->min) : 0);
    int tw = painter_text_width(p, s, -1), th = painter_text_height(p);
    if (th <= w->h)
        painter_text(p, (w->w - tw) / 2, (w->h - th) / 2, s, t->color[TC_TEXT]);
}

static void progress_destroy(struct widget *w)
{
    struct progress *pr = (struct progress *)w;
    if (pr->timer && w->app)
        app_timer_remove(w->app, pr->timer);
}

const struct widget_class progress_class = { "progress", sizeof(struct progress), progress_measure, NULL, progress_paint,
                                             NULL, progress_destroy };

void progress_set_pulse(struct widget *w, int pulse)
{
    struct progress *pr = (struct progress *)w;
    pr->pulse = pulse != 0;
    if (!pr->pulse && pr->timer) {
        app_timer_remove(w->app, pr->timer);
        pr->timer = NULL;
    }
    widget_invalidate(w);
}

struct widget *progress_new(struct widget *parent)
{
    struct widget *w = widget_new(&progress_class, parent);
    if (w) {
        w->max = 100;
        widget_set_stretch(w, 1, 0);
    }
    return w;
}
