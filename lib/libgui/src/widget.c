/* Widget tree, properties, signals, focus, event routing and the paint
 * pass with partial redraws. Windows are in window.c, layout of boxes
 * and grids in layout.c. */
#include <gui/app.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- tree ---- */

struct widget *widget_new(const struct widget_class *cls, struct widget *parent)
{
    size_t size = cls->size < sizeof(struct widget) ? sizeof(struct widget) : cls->size;
    struct widget *w = calloc(1, size);
    if (!w)
        return NULL;
    w->cls = cls;
    w->visible = 1;
    w->enabled = 1;
    w->row_span = w->col_span = 1;
    w->padding = -1;
    w->dirty = 1;
    w->needs_layout = 1;
    if (parent)
        widget_add(parent, w);
    return w;
}

static void set_window(struct widget *w, struct widget *window, struct app *app)
{
    w->window = window;
    w->app = app;
    for (struct widget *c = w->first; c; c = c->next)
        set_window(c, window, app);
}

void widget_add(struct widget *parent, struct widget *child)
{
    child->parent = parent;
    child->prev = parent->last;
    child->next = NULL;
    if (parent->last)
        parent->last->next = child;
    else
        parent->first = child;
    parent->last = child;
    set_window(child, parent->window ? parent->window : parent, parent->app);
    widget_relayout(parent);
}

void widget_remove(struct widget *child)
{
    struct widget *parent = child->parent;
    if (!parent)
        return;
    if (child->prev)
        child->prev->next = child->next;
    else
        parent->first = child->next;
    if (child->next)
        child->next->prev = child->prev;
    else
        parent->last = child->prev;
    child->parent = child->next = child->prev = NULL;
    struct window_state *ws = child->window ? window_state_of(child->window) : NULL;
    if (ws) {
        if (ws->focus == child) ws->focus = NULL;
        if (ws->capture == child) ws->capture = NULL;
        if (ws->hover == child) ws->hover = NULL;
        if (ws->drag_source == child) ws->drag_source = NULL;
        if (ws->drop_target == child) ws->drop_target = NULL;
    }
    widget_relayout(parent);
}

void widget_destroy(struct widget *w)
{
    while (w->first)
        widget_destroy(w->first);
    if (w->parent)
        widget_remove(w);
    widget_emit(w, "destroy", NULL);    /* owners drop their references */
    if (w->cls->destroy)
        w->cls->destroy(w);
    for (struct handler *h = w->handlers; h;) {
        struct handler *next = h->next;
        free(h);
        h = next;
    }
    free(w->id);
    free(w->text);
    free(w->tip);
    free(w);
}

struct widget *widget_find(struct widget *root, const char *id)
{
    if (root->id && strcmp(root->id, id) == 0)
        return root;
    for (struct widget *c = root->first; c; c = c->next) {
        struct widget *f = widget_find(c, id);
        if (f)
            return f;
    }
    return NULL;
}

void widget_set_id(struct widget *w, const char *id)
{
    free(w->id);
    w->id = id ? strdup(id) : NULL;
}

/* ---- properties ---- */

void widget_set_text(struct widget *w, const char *text)
{
    if (w->text && text && strcmp(w->text, text) == 0)
        return;
    free(w->text);
    w->text = strdup(text ? text : "");
    widget_relayout(w);
}

const char *widget_text(const struct widget *w)
{
    return w->text ? w->text : "";
}

void widget_set_value(struct widget *w, int value)
{
    if (w->max > w->min) {
        if (value < w->min) value = w->min;
        if (value > w->max) value = w->max;
    }
    if (w->value == value)
        return;
    w->value = value;
    widget_invalidate(w);
}

void widget_set_range(struct widget *w, int min, int max)
{
    w->min = min;
    w->max = max;
    widget_set_value(w, w->value);
    widget_invalidate(w);
}

void widget_set_visible(struct widget *w, int visible)
{
    if (w->visible == !!visible)
        return;
    w->visible = !!visible;
    if (w->parent)
        widget_relayout(w->parent);
}

void widget_set_enabled(struct widget *w, int enabled)
{
    w->enabled = !!enabled;
    widget_invalidate(w);
}

void widget_set_hint(struct widget *w, int pref_w, int pref_h)
{
    w->hint.pref_w = pref_w;
    w->hint.pref_h = pref_h;
    widget_relayout(w);
}

void widget_set_min(struct widget *w, int min_w, int min_h)
{
    w->hint.min_w = min_w;
    w->hint.min_h = min_h;
    widget_relayout(w);
}

void widget_set_max(struct widget *w, int max_w, int max_h)
{
    w->hint.max_w = max_w;
    w->hint.max_h = max_h;
    widget_relayout(w);
}

void widget_set_stretch(struct widget *w, int x, int y)
{
    w->stretch_x = x;
    w->stretch_y = y;
    widget_relayout(w);
}

void widget_set_align(struct widget *w, enum align x, enum align y)
{
    w->align_x = x;
    w->align_y = y;
    widget_relayout(w);
}

void widget_set_grid(struct widget *w, int row, int col, int row_span, int col_span)
{
    w->row = row;
    w->col = col;
    w->row_span = row_span > 0 ? row_span : 1;
    w->col_span = col_span > 0 ? col_span : 1;
    widget_relayout(w);
}

void widget_set_padding(struct widget *w, int padding)
{
    w->padding = padding;
    widget_relayout(w);
}

void widget_set_tip(struct widget *w, const char *tip)
{
    free(w->tip);
    w->tip = tip ? strdup(tip) : NULL;
}

void widget_set_icon(struct widget *w, const struct image *icon)
{
    w->icon = icon;
    widget_relayout(w);
}

void widget_set_accel(struct widget *w, int key, int mods)
{
    w->accel_key = key;
    w->accel_mods = mods;
}

const struct theme *widget_theme(const struct widget *w)
{
    return app_theme(w->app);
}

int widget_scale(const struct widget *w)
{
    const struct widget *window = w->window ? w->window : w;
    if (window->cls != &window_class)
        return 1;
    struct window_state *ws = window_state_of((struct widget *)window);
    struct gui_window *g = ws->win;
    if (ws->popup_win)
        for (const struct widget *p = w; p; p = p->parent)
            if (p == ws->popup)
                g = ws->popup_win;
    return g && g->scale > 0 ? g->scale : 1;
}

int widget_text_width(const struct widget *w, const struct font *font, const char *text, int n)
{
    int s = widget_scale(w);
    return (gfx_text_width_font_scaled(font ? font : widget_theme(w)->font, text, n, s) + s - 1) / s;
}

int widget_text_index(const struct widget *w, const struct font *font, const char *text, int n, int px)
{
    int s = widget_scale(w);
    return gfx_text_index_font_scaled(font ? font : widget_theme(w)->font, text, n, px * s, s);
}

/* ---- signals ---- */

void widget_connect(struct widget *w, const char *signal, signal_fn fn, void *arg)
{
    struct handler *h = calloc(1, sizeof *h);
    if (!h)
        return;
    h->name = signal;
    h->fn = fn;
    h->arg = arg;
    struct handler **pp = &w->handlers;
    while (*pp)
        pp = &(*pp)->next;
    *pp = h;
}

int widget_emit(struct widget *w, const char *signal, void *args)
{
    for (struct handler *h = w->handlers; h; h = h->next)
        if (strcmp(h->name, signal) == 0 && h->fn(w, args, h->arg))
            return 1;
    return 0;
}

/* ---- redraw and layout marks ---- */

void widget_invalidate(struct widget *w)
{
    w->dirty = 1;
    for (struct widget *p = w->parent; p && !p->child_dirty; p = p->parent)
        p->child_dirty = 1;
    if (w->window)
        w->window->child_dirty = 1;
}

void widget_relayout(struct widget *w)
{
    for (struct widget *p = w; p; p = p->parent)
        p->needs_layout = 1;
    widget_invalidate(w);
}

/* ---- geometry helpers ---- */

void widget_abs(const struct widget *w, int *x, int *y)
{
    *x = 0;
    *y = 0;
    for (const struct widget *p = w; p && p->parent; p = p->parent) {
        *x += p->x;
        *y += p->y;
    }
}

void widget_text_cursor(struct widget *w, int x, int y, int width, int height)
{
    struct window_state *ws = w->window ? window_state_of(w->window) : NULL;
    if (!ws || !ws->win || ws->focus != w)
        return;
    int ax, ay;
    widget_abs(w, &ax, &ay);
    gui_text_input_set_cursor(ws->win, ax + x, ay + y, width, height);
}

struct widget *widget_at(struct widget *w, int x, int y)
{
    if (!w->visible || x < 0 || y < 0 || x >= w->w || y >= w->h)
        return NULL;
    for (struct widget *c = w->last; c; c = c->prev) {
        struct widget *hit = widget_at(c, x - c->x, y - c->y);
        if (hit)
            return hit;
    }
    return w;
}

/* ---- focus ---- */

static int can_focus(const struct widget *w)
{
    for (const struct widget *p = w; p; p = p->parent)
        if (!p->visible || !p->enabled)
            return 0;
    return w->focusable;
}

void widget_focus(struct widget *w)
{
    if (!w->window)
        return;
    struct window_state *ws = window_state_of(w->window);
    if (ws->focus == w)
        return;
    if (w && !can_focus(w))
        return;
    struct widget *old = ws->focus;
    ws->focus = w;
    if (ws->win)
        gui_text_input_set(ws->win, w && w->accepts_text);
    if (old) {
        old->focused = 0;
        struct event e = { .type = EV_FOCUS_OUT };
        if (old->cls->event)
            old->cls->event(old, &e);
        widget_invalidate(old);
    }
    if (w) {
        w->focused = 1;
        struct event e = { .type = EV_FOCUS_IN };
        if (w->cls->event)
            w->cls->event(w, &e);
        widget_invalidate(w);
    }
}

struct widget *widget_focused(struct widget *window)
{
    return window_state_of(window)->focus;
}

void widget_capture(struct widget *w)
{
    if (w->window)
        window_state_of(w->window)->capture = w;
}

/* Next focusable widget after w in tree order (wrapping), or before
 * it when backwards. */
static struct widget *next_in_order(struct widget *w, int backwards)
{
    if (!backwards) {
        if (w->first)
            return w->first;
        for (; w; w = w->parent)
            if (w->next)
                return w->next;
        return NULL;
    }
    if (w->prev) {
        w = w->prev;
        while (w->last)
            w = w->last;
        return w;
    }
    return w->parent;
}

static struct widget *last_in_tree(struct widget *w)
{
    while (w->last)
        w = w->last;
    return w;
}

void widget_focus_next(struct widget *window, int backwards);
void widget_focus_next(struct widget *window, int backwards)
{
    struct window_state *ws = window_state_of(window);
    struct widget *start = ws->focus ? ws->focus : window;
    struct widget *w = start;
    for (int guard = 0; guard < 10000; guard++) {
        w = next_in_order(w, backwards);
        if (!w)
            w = backwards ? last_in_tree(window) : window;
        if (w == start)
            break;
        if (w != window && can_focus(w)) {
            widget_focus(w);
            return;
        }
    }
}

/* ---- event dispatch ---- */

int widget_dispatch(struct widget *w, struct event *e)
{
    for (struct widget *t = w; t; t = t->parent) {
        if (!t->enabled)
            return 0;
        if (t->cls->event && t->cls->event(t, e))
            return 1;
        /* Translate mouse coordinates into the parent's space. */
        e->x += t->x;
        e->y += t->y;
    }
    return 0;
}
