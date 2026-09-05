/* Top level windows: server messages become widget events, focus and
 * hover tracking, Tab traversal, accelerators and mnemonics, and the
 * layout and paint pass with partial redraws. */
#include <gui/app.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct window {
    struct widget w;
    struct window_state st;
};

void widget_measure(struct widget *w);
void widget_focus_next(struct widget *window, int backwards);

struct window_state *window_state_of(struct widget *window)
{
    return &((struct window *)window)->st;
}

/* ---- layout and paint ---- */

static void layout_tree(struct widget *w)
{
    if (w->cls->layout)
        w->cls->layout(w);
    w->needs_layout = 0;
    for (struct widget *c = w->first; c; c = c->next)
        if (c->visible)
            layout_tree(c);
}

static void paint_tree(struct widget *w, struct painter *p, int force, struct rect *damage, int *has, struct widget *skip)
{
    if (!w->visible || w == skip)
        return;
    painter_push(p, w->x, w->y, w->w, w->h);
    if (force || w->dirty) {
        if (w->cls->paint)
            w->cls->paint(w, p);
        struct rect r = p->clip;
        if (!rect_empty(r)) {
            *damage = *has ? rect_union(*damage, r) : r;
            *has = 1;
        }
        for (struct widget *c = w->first; c; c = c->next)
            paint_tree(c, p, 1, damage, has, skip);
    } else if (w->child_dirty) {
        for (struct widget *c = w->first; c; c = c->next)
            if (c->dirty || c->child_dirty)
                paint_tree(c, p, 0, damage, has, skip);
    }
    w->dirty = 0;
    w->child_dirty = 0;
    painter_pop(p);
}

/* The painter's clip is in device pixels; damage is reported in logical
 * pixels, rounded outwards. */
static struct rect device_to_logical(struct rect r, int scale)
{
    if (scale <= 1)
        return r;
    int x0 = r.x / scale, y0 = r.y / scale;
    int x1 = (r.x + r.w + scale - 1) / scale, y1 = (r.y + r.h + scale - 1) / scale;
    return (struct rect){ x0, y0, x1 - x0, y1 - y0 };
}

struct rect window_paint(struct widget *window)
{
    struct window_state *ws = window_state_of(window);
    struct rect none = { 0, 0, 0, 0 };
    if (!ws->win || ws->closed)
        return none;
    if (window->needs_layout) {
        widget_measure(window);
        window->x = window->y = 0;
        window->w = ws->win->width;
        window->h = ws->win->height;
        layout_tree(window);
        window->dirty = 1;
    }
    if (!window->dirty && !window->child_dirty)
        return none;
    struct painter p;
    int scale = ws->win->scale > 0 ? ws->win->scale : 1;
    painter_init_scaled(&p, &ws->win->surf, app_theme(window->app), scale);
    struct rect damage = none;
    int has = 0;
    paint_tree(window, &p, window->dirty, &damage, &has, ws->popup_win ? ws->popup : NULL);
    if (has) {
        damage = device_to_logical(damage, scale);
        gui_damage(ws->win, damage.x, damage.y, damage.w, damage.h);
    }
    if (ws->popup_win && ws->popup) {
        struct painter pp;
        int pscale = ws->popup_win->scale > 0 ? ws->popup_win->scale : 1;
        painter_init_scaled(&pp, &ws->popup_win->surf, app_theme(window->app), pscale);
        struct rect pd = none;
        int phas = 0;
        paint_tree(ws->popup, &pp, 1, &pd, &phas, NULL);
        if (phas) {
            pd = device_to_logical(pd, pscale);
            gui_damage(ws->popup_win, pd.x, pd.y, pd.w, pd.h);
        }
    }
    return has ? damage : none;
}

/* ---- events ---- */

/* ---- popups and tooltips ---- */

static void popup_open(struct widget *window, struct widget *w, int x, int y, int width, int height, int grab)
{
    window_popup_close(window);
    struct window_state *ws = window_state_of(window);
    w->floating = 1;
    if (w->parent != window)
        widget_add(window, w);
    ws->popup_win = gui_has_popup_surfaces() && ws->win ? gui_create_popup_window(ws->win, x, y, width, height, grab) : NULL;
    w->x = ws->popup_win ? 0 : x;
    w->y = ws->popup_win ? 0 : y;
    w->w = width;
    w->h = height;
    if (!ws->popup_win) {
        if (x + width > window->w) w->x = window->w - width;
        if (y + height > window->h) w->y = window->h - height;
        if (w->x < 0) w->x = 0;
        if (w->y < 0) w->y = 0;
    }
    ws->popup = w;
    widget_relayout(w);
    widget_invalidate(window);
}

void window_popup_open(struct widget *window, struct widget *w, int x, int y, int width, int height)
{
    popup_open(window, w, x, y, width, height, 1);
}

void window_popup_close(struct widget *window)
{
    struct window_state *ws = window_state_of(window);
    if (!ws->popup)
        return;
    struct widget *p = ws->popup;
    ws->popup = NULL;
    if (ws->popup_win) {
        gui_destroy_window(ws->popup_win);
        ws->popup_win = NULL;
    }
    widget_destroy(p);
    widget_invalidate(window);
}

static int inside(struct widget *ancestor, struct widget *w)
{
    for (; w; w = w->parent)
        if (w == ancestor)
            return 1;
    return 0;
}

static void tip_hide(struct widget *window)
{
    struct window_state *ws = window_state_of(window);
    if (ws->tip_timer) {
        app_timer_remove(window->app, ws->tip_timer);
        ws->tip_timer = NULL;
    }
    if (ws->tip) {
        if (ws->tip == ws->popup) {
            ws->tip = NULL;
            window_popup_close(window);
        } else {
            widget_destroy(ws->tip);
            ws->tip = NULL;
            widget_invalidate(window);
        }
    }
    ws->tip_owner = NULL;
}

static void tip_show(void *arg)
{
    struct widget *window = arg;
    struct window_state *ws = window_state_of(window);
    ws->tip_timer = NULL;
    struct widget *o = ws->tip_owner;
    if (!o || !o->tip || ws->popup)
        return;
    struct widget *l = label_new(NULL, o->tip);
    const struct theme *t = app_theme(window->app);
    int ax, ay;
    widget_abs(o, &ax, &ay);
    l->w = gfx_text_width_font(t->font, o->tip, -1) + 8;
    l->h = t->font->height + 6;
    l->value = 1;                       /* label paints as a tooltip */
    popup_open(window, l, ax, ay + o->h + 2, l->w, l->h, 0);
    ws->tip = l;
    widget_invalidate(l);
}

static void set_hover(struct widget *window, struct widget *w)
{
    struct window_state *ws = window_state_of(window);
    if (ws->hover == w)
        return;
    tip_hide(window);
    if (w && w->tip && window->app) {
        ws->tip_owner = w;
        ws->tip_timer = app_timer_add(window->app, 600, 0, tip_show, window);
    }
    struct event e = { .type = EV_LEAVE };
    if (ws->hover) {
        ws->hover->hover = 0;
        if (ws->hover->cls->event)
            ws->hover->cls->event(ws->hover, &e);
    }
    ws->hover = w;
    if (w) {
        w->hover = 1;
        e.type = EV_ENTER;
        if (w->cls->event)
            w->cls->event(w, &e);
    }
}

static void mouse_message(struct widget *window, struct wmsg *m)
{
    struct window_state *ws = window_state_of(window);
    int from_popup = ws->popup_win && m->window == ws->popup_win->id;
    struct widget *root = from_popup ? ws->popup : window;
    struct widget *target = ws->capture ? ws->capture : widget_at(root, m->a, m->b);
    if (ws->tip && target == ws->tip)
        target = NULL;
    if (m->d == WMOUSE_MOVE || m->d == WMOUSE_DOWN)
        set_hover(window, ws->capture ? ws->capture : target);
    if (m->d == WMOUSE_DOWN) {
        tip_hide(window);
        if (ws->popup && !from_popup && !inside(ws->popup, target)) {
            window_popup_close(window);
            return;
        }
    }
    if (!target)
        return;
    int ax, ay;
    widget_abs(target, &ax, &ay);
    if (from_popup && ws->popup) {
        int px, py;
        widget_abs(ws->popup, &px, &py);
        ax -= px;
        ay -= py;
    }
    struct event e = { .x = m->a - ax, .y = m->b - ay, .button = m->c, .mods = 0 };
    switch (m->d) {
    case WMOUSE_DOWN:
        e.type = EV_MOUSE_DOWN;
        if (target->focusable)
            widget_focus(target);
        break;
    case WMOUSE_UP:
        e.type = EV_MOUSE_UP;
        break;
    case WMOUSE_WHEEL:
        e.type = EV_MOUSE_WHEEL;
        e.button = m->c;
        break;
    default:
        e.type = EV_MOUSE_MOVE;
        break;
    }
    widget_dispatch(target, &e);
    if (m->d == WMOUSE_UP && !(m->c & 1))
        ws->capture = NULL;
}

static int mnemonic_of(const struct widget *w)
{
    const char *t = w->text;
    if (!t)
        return 0;
    const char *amp = strchr(t, '&');
    if (!amp || !amp[1])
        return 0;
    int c = (unsigned char)amp[1];
    return c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c;
}

/* Accelerators of items in closed menus stay active; their mnemonics
 * do not (those belong to the open menu). */
static int activate_accel(struct widget *w, int code, int ch, int mods, int in_closed_menu)
{
    int menu = w->cls == &menu_class, item = w->cls == &menuitem_class;
    if (!w->enabled || (!w->visible && !menu && !(item && in_closed_menu)))
        return 0;
    if (menu && !w->visible)
        in_closed_menu = 1;
    if ((w->accel_key && w->accel_key == code && w->accel_mods == mods) ||
        (!in_closed_menu && (mods & WMOD_ALT) && ch && mnemonic_of(w) == ch)) {
        struct sig_click c = { 1, 0, 0 };
        widget_emit(w, "clicked", &c);
        return 1;
    }
    for (struct widget *c = w->first; c; c = c->next)
        if (activate_accel(c, code, ch, mods, in_closed_menu))
            return 1;
    return 0;
}

static void key_message(struct widget *window, struct wmsg *m)
{
    struct window_state *ws = window_state_of(window);
    struct event e = { .type = m->b ? EV_KEY_DOWN : EV_KEY_UP, .code = m->a, .ch = m->d, .mods = m->c };
    if (m->b)
        tip_hide(window);
    if (m->b && m->a == KEY_ESC && ws->popup) {
        window_popup_close(window);
        return;
    }
    struct widget *target = ws->focus ? ws->focus : window;
    if (ws->popup && !inside(ws->popup, target))
        target = ws->popup;
    if (widget_dispatch(target, &e))
        return;
    if (!m->b)
        return;
    if (m->a == KEY_TAB && !(m->c & (WMOD_CTRL | WMOD_ALT))) {
        widget_focus_next(window, m->c & WMOD_SHIFT);
        return;
    }
    if (activate_accel(window, m->a, m->d, m->c, 0))
        return;
    struct sig_key k = { m->a, m->d, m->c };
    widget_emit(window, "key", &k);
}

static void text_message(struct widget *window, struct wmsg *m, enum event_type type)
{
    struct window_state *ws = window_state_of(window);
    struct widget *target = ws->focus ? ws->focus : window;
    if (ws->popup && !inside(ws->popup, target))
        target = ws->popup;
    struct event e = { .type = type, .text = m->text, .before = m->a, .after = m->b };
    widget_dispatch(target, &e);
}

void window_message(struct widget *window, struct wmsg *m)
{
    struct window_state *ws = window_state_of(window);
    switch (m->type) {
    case WM_MOUSE:
        mouse_message(window, m);
        break;
    case WM_KEY:
        key_message(window, m);
        break;
    case WM_TEXT:
        text_message(window, m, EV_TEXT);
        break;
    case WM_PREEDIT:
        text_message(window, m, EV_PREEDIT);
        break;
    case WM_TEXT_DELETE:
        text_message(window, m, EV_TEXT_DELETE);
        break;
    case WM_FOCUS: {
        struct sig_change c = { m->a, NULL };
        widget_emit(window, "focus", &c);
        break;
    }
    case WM_RESIZED: {
        if (ws->popup_win && m->window == ws->popup_win->id) {
            ws->popup->w = m->a;
            ws->popup->h = m->b;
            widget_relayout(ws->popup);
            break;
        }
        widget_relayout(window);
        struct sig_resize r = { m->a, m->b };
        widget_emit(window, "resize", &r);
        break;
    }
    case WM_CLOSE:
        if (ws->popup_win && m->window == ws->popup_win->id) {
            window_popup_close(window);
            break;
        }
        if (!widget_emit(window, "close", NULL))
            window_close(window);
        break;
    }
}

int window_owns_id(struct widget *window, int id)
{
    struct window_state *ws = window_state_of(window);
    return (ws->win && ws->win->id == id) || (ws->popup_win && ws->popup_win->id == id);
}

void window_close(struct widget *window)
{
    window_state_of(window)->closed = 1;
}

/* ---- class ---- */

static void window_paint_bg(struct widget *w, struct painter *p)
{
    painter_fill(p, 0, 0, w->w, w->h, p->theme->color[TC_WINDOW]);
}

static void window_measure(struct widget *w, struct size_hint *h)
{
    /* A window is a vertical box around its children. */
    extern const struct widget_class box_class;
    int saved = w->value;
    w->value = 1;
    box_class.measure(w, h);
    w->value = saved;
}

static void window_layout(struct widget *w)
{
    extern const struct widget_class box_class;
    int saved = w->value;
    w->value = 1;
    box_class.layout(w);
    w->value = saved;
}

static void window_destroy(struct widget *w)
{
    struct window_state *ws = window_state_of(w);
    if (ws->tip_timer && w->app)
        app_timer_remove(w->app, ws->tip_timer);
    if (ws->popup_win)
        gui_destroy_window(ws->popup_win);
    if (ws->win)
        gui_destroy_window(ws->win);
    ws->win = NULL;
}

const struct widget_class window_class = {
    "window", sizeof(struct window), window_measure, window_layout, window_paint_bg, NULL, window_destroy,
};
