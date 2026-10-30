/* Top level windows: server messages become widget events, focus and
 * hover tracking, Tab traversal, accelerators and mnemonics, drag and
 * drop, and the layout and paint pass with partial redraws. */
#include <gui/app.h>
#include <gui/image.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

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

/* Lays out w when force is set. The pass then descends into the children
 * that moved, that need a layout or that have descendants that need one.
 * A container whose children moved is repainted, because the areas that
 * the children left show the container. */
static void layout_pass(struct widget *w, int force)
{
    if (force) {
        gui_count(GUI_COUNT_LAYOUTS, 1);
        if (w->cls->layout)
            w->cls->layout(w);
    }
    w->needs_layout = 0;
    w->child_layout = 0;
    int moved = 0;
    for (struct widget *c = w->first; c; c = c->next) {
        int f = c->moved || c->needs_layout;
        moved |= c->moved;
        c->moved = 0;
        if (c->visible && (f || c->child_layout))
            layout_pass(c, f);
    }
    if (force && moved)
        widget_invalidate(w);
}

/* Marks every widget of the tree for a new measurement and layout. */
static void mark_all(struct widget *w)
{
    w->needs_measure = 1;
    w->needs_layout = 1;
    for (struct widget *c = w->first; c; c = c->next)
        mark_all(c);
}

void window_relayout_all(struct widget *window)
{
    window_state_of(window)->relayout_all = 1;
    widget_invalidate(window);
}

/* Clears the paint marks of a subtree that the paint skipped. Without
 * this, a mark below would stop the propagation of later invalidations. */
static void clear_marks(struct widget *w)
{
    w->dirty = 0;
    w->dirty_part = 0;
    w->scroll_dy = 0;
    if (!w->child_dirty)
        return;
    w->child_dirty = 0;
    for (struct widget *c = w->first; c; c = c->next)
        clear_marks(c);
}

/* Performs the pending move of widget_scroll_area. The moved area is
 * damage for the server. An area that is not wholly visible is repainted
 * instead, because its hidden pixels are not in the surface. */
static void scroll_pixels(struct widget *w, struct painter *p, struct rect_set *rects)
{
    int s = p->scale;
    struct rect r = w->scroll_rect;
    struct rect dev = { p->ox + r.x * s, p->oy + r.y * s, r.w * s, r.h * s };
    int dy = w->scroll_dy * s;
    w->scroll_dy = 0;
    struct rect seen = rect_intersect(dev, p->clip);
    if (seen.x != dev.x || seen.y != dev.y || seen.w != dev.w || seen.h != dev.h) {
        widget_invalidate_rect(w, r);
        return;
    }
    gfx_move_rect(p->s, dev, 0, dy);
    rect_set_add(rects, dev);
}

/* Paints the dirty widgets of the tree. The device rectangle of each
 * repainted area goes into rects. The set merges close rectangles, so the
 * server composes only the repainted parts and not their bounding box.
 * force is set below a repainted widget. The rectangle of that widget already contains
 * the children. A widget with dirty_part repaints its dirty rectangle:
 * its own paint and the children within the rectangle. */
static void paint_tree(struct widget *w, struct painter *p, int force, struct rect_set *rects, struct widget *skip)
{
    if (!w->visible || w == skip)
        return;
    painter_push(p, w->x, w->y, w->w, w->h);
    if (rect_empty(p->clip)) {
        /* Outside the clip, for example a row of a scrolled area. */
        clear_marks(w);
        painter_pop(p);
        return;
    }
    if (!force && !w->dirty) {
        /* A transparent child marked dirty directly needs the background
         * of this widget below it. */
        for (struct widget *c = w->first; c; c = c->next)
            if (c->transparent && c->dirty && c->visible) {
                c->dirty = 0;
                widget_invalidate_rect(w, (struct rect){ c->x, c->y, c->w, c->h });
            }
    }
    if (force || w->dirty) {
        w->scroll_dy = 0;
        if (w->cls->paint) {
            w->cls->paint(w, p);
            gui_count(GUI_COUNT_WIDGET_PAINTS, 1);
        }
        struct rect r = p->clip;
        if (!rect_empty(r) && !force)
            rect_set_add(rects, r);
        for (struct widget *c = w->first; c; c = c->next)
            paint_tree(c, p, 1, rects, skip);
    } else {
        if (w->scroll_dy)
            scroll_pixels(w, p, rects);
        /* The marked children first. A part of them inside the dirty
         * rectangle is painted again below, over the background. */
        if (w->child_dirty)
            for (struct widget *c = w->first; c; c = c->next)
                if (c->dirty || c->child_dirty || c->dirty_part)
                    paint_tree(c, p, 0, rects, skip);
        for (int i = 0; w->dirty_part && i < w->ndirty; i++) {
            struct rect d = w->dirty_rects[i];
            painter_push_clip(p, d.x, d.y, d.w, d.h);
            if (!rect_empty(p->clip)) {
                if (w->cls->paint) {
                    w->cls->paint(w, p);
                    gui_count(GUI_COUNT_WIDGET_PAINTS, 1);
                }
                rect_set_add(rects, p->clip);
                for (struct widget *c = w->first; c; c = c->next)
                    paint_tree(c, p, 1, rects, skip);
            }
            painter_pop(p);
        }
    }
    w->dirty = 0;
    w->dirty_part = 0;
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

void window_set_header(struct widget *window, struct widget *w)
{
    struct window_state *ws = window_state_of(window);
    if (!ws->win || ws->header || w->parent != window || gui_window_set_header_slot(ws->win, 0, NULL) < 0)
        return;
    ws->header = w;
    w->floating = 1;
    widget_relayout(window);
}

/* Gives the header slot the preferred width of the header widget and
 * places the widget on the slot. Without a header widget the slot is
 * removed. */
static void header_place(struct widget *window)
{
    struct window_state *ws = window_state_of(window);
    struct widget *h = ws->header;
    int width = 0;
    if (h && h->visible) {
        widget_measure(h);
        width = h->measured.pref_w;
    }
    struct rect slot;
    if (gui_window_set_header_slot(ws->win, width, &slot) < 0 || !h)
        return;
    if (h->x != slot.x || h->y != slot.y || h->w != slot.w || h->h != slot.h) {
        h->x = slot.x;
        h->y = slot.y;
        h->w = slot.w;
        h->h = slot.h;
        widget_invalidate(h);
    }
}

/* Paints the header widget into the header slot. The rectangles of the
 * slot surface go to the server as damage in window coordinates. force is
 * set when the whole window is repainted. */
static struct rect header_paint(struct widget *window, int force)
{
    struct window_state *ws = window_state_of(window);
    struct widget *h = ws->header;
    struct rect damage = { 0, 0, 0, 0 };
    if (!h || !h->visible || (!force && !h->dirty && !h->child_dirty && !h->dirty_part))
        return damage;
    struct surface hs = gui_window_header_surface(ws->win);
    if (!hs.pixels) {
        clear_marks(h);
        return damage;
    }
    int scale = ws->win->scale > 0 ? ws->win->scale : 1;
    struct painter p;
    painter_init_scaled(&p, &hs, app_theme(window->app), scale);
    /* The surface begins at the top left corner of the slot. */
    p.ox = -h->x * scale;
    p.oy = -h->y * scale;
    struct rect_set rects = { 0 };
    paint_tree(h, &p, force, &rects, NULL);
    /* A forced paint records no rectangles. The damage is the slot. */
    if (force)
        rect_set_add(&rects, (struct rect){ 0, 0, hs.width, hs.height });
    for (int i = 0; i < rects.n; i++) {
        gui_count(GUI_COUNT_PAINTED_PIXELS, (uint64_t)rects.r[i].w * rects.r[i].h);
        struct rect r = device_to_logical(rects.r[i], scale);
        r.x += h->x;
        r.y += h->y;
        gui_damage(ws->win, r.x, r.y, r.w, r.h);
        damage = i ? rect_union(damage, r) : r;
    }
    return damage;
}

struct rect window_paint(struct widget *window)
{
    struct window_state *ws = window_state_of(window);
    struct rect none = { 0, 0, 0, 0 };
    if (!ws->win || ws->closed)
        return none;
    if (ws->relayout_all) {
        ws->relayout_all = 0;
        mark_all(window);
        window->dirty = 1;
    }
    header_place(window);
    if (window->needs_measure || window->needs_layout || window->child_layout) {
        widget_measure(window);
        if (window->w != ws->win->width || window->h != ws->win->height) {
            window->needs_layout = 1;
            window->dirty = 1;
        }
        window->x = window->y = 0;
        window->w = ws->win->width;
        window->h = ws->win->height;
        layout_pass(window, window->needs_layout);
    }
    if (!window->dirty && !window->child_dirty)
        return none;
    gui_begin_paint(ws->win);
    long t0 = uptime_us();
    struct painter p;
    int scale = ws->win->scale > 0 ? ws->win->scale : 1;
    painter_init_scaled(&p, &ws->win->surf, app_theme(window->app), scale);
    struct rect_set rects = { 0 };
    /* The header widget lies outside the contents. The paint of the tree
     * finds an empty clip there and only clears its marks, so the header
     * is painted first. */
    struct rect damage = header_paint(window, window->dirty);
    int headed = !rect_empty(damage);
    paint_tree(window, &p, 0, &rects, ws->popup_win ? ws->popup : NULL);
    for (int i = 0; i < rects.n; i++) {
        gui_count(GUI_COUNT_PAINTED_PIXELS, (uint64_t)rects.r[i].w * rects.r[i].h);
        struct rect r = device_to_logical(rects.r[i], scale);
        gui_damage(ws->win, r.x, r.y, r.w, r.h);
        damage = i || headed ? rect_union(damage, r) : r;
    }
    if (ws->popup_win && ws->popup) {
        gui_begin_paint(ws->popup_win);
        struct painter pp;
        int pscale = ws->popup_win->scale > 0 ? ws->popup_win->scale : 1;
        painter_init_scaled(&pp, &ws->popup_win->surf, app_theme(window->app), pscale);
        /* The popup is painted whole, and its damage is the surface. */
        struct rect_set prects = { 0 };
        paint_tree(ws->popup, &pp, 1, &prects, NULL);
        if (ws->popup_nshape)
            painter_round_alpha(&pp, ws->popup_shape, ws->popup_nshape, ws->popup_radius,
                                app_theme(window->app)->color[TC_BORDER]);
        if (ws->popup->w > 0 && ws->popup->h > 0) {
            gui_damage(ws->popup_win, 0, 0, ws->popup->w, ws->popup->h);
            gui_count(GUI_COUNT_PAINTED_PIXELS, (uint64_t)pp.s->width * pp.s->height);
        }
    }
    if (rects.n || headed)
        gui_count_paint(uptime_us() - t0);
    return damage;
}

/* ---- events ---- */

/* ---- popups and tooltips ---- */

static void popup_open(struct widget *window, struct widget *w, int x, int y, int width, int height, int grab)
{
    window_popup_close(window);
    struct window_state *ws = window_state_of(window);
    ws->popup_nshape = 0;
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

void window_popup_set_shape(struct widget *window, const struct rect *r, int n, int radius)
{
    struct window_state *ws = window_state_of(window);
    if (!ws->popup || n < 1 || n > 2)
        return;
    memcpy(ws->popup_shape, r, (size_t)n * sizeof *r);
    ws->popup_nshape = n;
    ws->popup_radius = radius;
    if (!ws->popup_win)
        return;
    gui_set_translucent(ws->popup_win);
    struct rect region[6];
    int count = 0;
    for (int i = 0; i < n; i++)
        count += gui_rounded_region(r[i], radius, region + count);
    gui_set_opaque_region(ws->popup_win, region, count);
    widget_invalidate(ws->popup);
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

/* The tooltip: the text in a rounded box of the highlight colour. The
 * padding and the radius follow the theme. */
static int tip_pad_x(const struct theme *t) { return theme_scale_px(t, 6); }
static int tip_pad_y(const struct theme *t) { return theme_scale_px(t, 3); }

static void tooltip_paint(struct widget *w, struct painter *p)
{
    const struct theme *t = p->theme;
    painter_rounded(p, 0, 0, w->w, w->h, t->color[TC_HIGHLIGHT], t->color[TC_BORDER]);
    painter_text(p, tip_pad_x(t), tip_pad_y(t), widget_text(w), t->color[TC_TEXT]);
}

static const struct widget_class tooltip_class = { "tooltip", sizeof(struct widget), NULL, NULL, tooltip_paint, NULL,
                                                   NULL, NULL };

static void tip_show(void *arg)
{
    struct widget *window = arg;
    struct window_state *ws = window_state_of(window);
    ws->tip_timer = NULL;
    struct widget *o = ws->tip_owner;
    if (!o || !o->tip || ws->popup)
        return;
    struct widget *l = widget_new(&tooltip_class, NULL);
    if (!l)
        return;
    widget_set_text(l, o->tip);
    const struct theme *t = app_theme(window->app);
    int ax, ay;
    widget_abs(o, &ax, &ay);
    l->w = widget_text_width(window, NULL, o->tip, -1) + 2 * tip_pad_x(t);
    l->h = t->font->height + 2 * tip_pad_y(t);
    popup_open(window, l, ax, ay + o->h + 2, l->w, l->h, 0);
    ws->tip = l;
    widget_invalidate(l);
}

/* ---- the caret blink ---- */

#define BLINK_FOR_MS 10000

static void caret_invalidate(struct window_state *ws)
{
    struct widget *o = ws->caret_owner;
    if (o && o == ws->focus) {
        struct rect r = ws->caret_rect;
        widget_invalidate_rect(o, (struct rect){ r.x - 1, r.y, r.w + 2, r.h });
    }
}

static void blink(void *arg)
{
    struct widget *window = arg;
    struct window_state *ws = window_state_of(window);
    int done = !ws->focus || !ws->focus->accepts_text || uptime_ms() - ws->last_input_ms >= BLINK_FOR_MS;
    if (done) {
        app_timer_remove(window->app, ws->blink_timer);
        ws->blink_timer = NULL;
        if (!ws->caret_hidden)
            return;
        ws->caret_hidden = 0;
    } else {
        ws->caret_hidden = !ws->caret_hidden;
    }
    caret_invalidate(ws);
}

/* Input shows the caret and restarts the blink of a focused text widget. */
static void caret_input(struct widget *window)
{
    struct window_state *ws = window_state_of(window);
    ws->last_input_ms = uptime_ms();
    if (ws->caret_hidden) {
        ws->caret_hidden = 0;
        caret_invalidate(ws);
    }
    if (!ws->blink_timer && window->app && ws->focus && ws->focus->accepts_text)
        ws->blink_timer = app_timer_add(window->app, GUI_CARET_BLINK_MS, 1, blink, window);
}

void window_caret_restart(struct widget *window)
{
    caret_input(window);
}

void window_set_translucent(struct widget *window)
{
    struct window_state *ws = window_state_of(window);
    ws->translucent = 1;
    if (ws->win)
        gui_set_translucent(ws->win);
    widget_invalidate(window);
}

int widget_caret_visible(const struct widget *w)
{
    struct window_state *ws = w->window ? window_state_of(w->window) : NULL;
    return !ws || ws->focus != w || !ws->caret_hidden;
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

/* The widget at window coordinates (x, y), in the header slot too. */
static struct widget *window_widget_at(struct widget *window, int x, int y)
{
    struct widget *h = window_state_of(window)->header;
    if (h && h->visible && rect_contains((struct rect){ h->x, h->y, h->w, h->h }, x, y))
        return widget_at(h, x - h->x, y - h->y);
    return widget_at(window, x, y);
}

void window_update_cursor(struct widget *window)
{
    struct window_state *ws = window_state_of(window);
    int popup = ws->mouse_popup && ws->popup;
    struct gui_window *gw = popup ? ws->popup_win : ws->win;
    if (!gw)
        return;
    struct widget *target = ws->capture ? ws->capture
                            : popup ? widget_at(ws->popup, ws->mouse_x, ws->mouse_y)
                                    : window_widget_at(window, ws->mouse_x, ws->mouse_y);
    const char *name = "default";
    if (target) {
        int ax, ay;
        widget_abs(target, &ax, &ay);
        if (popup) {
            int px, py;
            widget_abs(ws->popup, &px, &py);
            ax -= px;
            ay -= py;
        }
        name = widget_cursor_at(target, ws->mouse_x - ax, ws->mouse_y - ay);
    }
    gui_window_set_cursor(gw, name);
}

/* The focus mark appears after keyboard input and disappears at a button
 * press, so a click on a control marks the control by its state alone. */
static void set_focus_visible(struct widget *window, int visible)
{
    struct window_state *ws = window_state_of(window);
    if (ws->focus_visible == visible)
        return;
    ws->focus_visible = visible;
    if (ws->focus)
        widget_invalidate(ws->focus);
}

static void mouse_message(struct widget *window, struct wmsg *m)
{
    struct window_state *ws = window_state_of(window);
    int from_popup = ws->popup_win && m->window == ws->popup_win->id;
    if (m->d == WMOUSE_DOWN) {
        caret_input(window);
        set_focus_visible(window, 0);
    }
    struct widget *target = ws->capture ? ws->capture
                            : from_popup ? widget_at(ws->popup, m->a, m->b)
                                         : window_widget_at(window, m->a, m->b);
    if (ws->tip && target == ws->tip)
        target = NULL;
    if (m->d == WMOUSE_MOVE || m->d == WMOUSE_DOWN)
        set_hover(window, ws->capture ? ws->capture : target);
    if (m->d == WMOUSE_MOVE || m->d == WMOUSE_DOWN || m->d == WMOUSE_UP) {
        ws->mouse_x = m->a;
        ws->mouse_y = m->b;
        ws->mouse_popup = from_popup;
        window_update_cursor(window);
    }
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

/* ---- drag and drop ---- */

int widget_drag_moved(int press_x, int press_y, int x, int y)
{
    int dx = x - press_x, dy = y - press_y;
    return dx * dx + dy * dy > DRAG_THRESHOLD * DRAG_THRESHOLD;
}

int gui_click_count(struct gui_clicks *c, int x, int y)
{
    long now = uptime_ms();
    int repeat = c->count && now - c->ms < GUI_DOUBLE_CLICK_MS && !widget_drag_moved(c->x, c->y, x, y);
    c->count = repeat ? c->count % 3 + 1 : 1;
    c->ms = now;
    c->x = x;
    c->y = y;
    return c->count;
}

int widget_drag_offers(const char *mime) { return gui_drag_offers(mime); }

/* The drag image: a rounded tile with the icon and the label, slightly
 * transparent, in device pixels at the window's scale. */
static struct surface drag_image(struct widget *window, const struct image *icon, const char *label, int *lw, int *lh)
{
    struct window_state *ws = window_state_of(window);
    const struct theme *t = app_theme(window->app);
    int scale = ws->win->scale > 0 ? ws->win->scale : 1;
    int iw = icon ? image_lw(icon) : 0, ih = icon ? image_lh(icon) : 0;
    if (iw > 20 || ih > 20)
        iw = ih = 20;
    int tw = label && *label ? widget_text_width(window, NULL, label, -1) : 0;
    int w = 8 + iw + (iw && tw ? 6 : 0) + tw + 8, h = (ih > t->font->height ? ih : t->font->height) + 10;
    if (w > 240)
        w = 240;
    if (w < 16)
        w = 16;
    struct surface s = { calloc((size_t)w * scale * h * scale, 4), w * scale, h * scale, w * scale };
    if (!s.pixels)
        return s;
    /* Pixels the tile does not cover remain the marker and become transparent. */
    const uint32_t marker = 0x00ff00ff;
    for (size_t i = 0; i < (size_t)s.width * s.height; i++)
        s.pixels[i] = marker;
    struct painter p;
    painter_init_scaled(&p, &s, t, scale);
    painter_rounded(&p, 0, 0, w, h, t->color[TC_FIELD], t->color[TC_BORDER]);
    int x = 8;
    if (icon) {
        painter_image_scaled(&p, x, (h - ih) / 2, iw, ih, icon);
        x += iw + 6;
    }
    if (tw)
        painter_text(&p, x, (h - t->font->height) / 2, label, t->color[TC_TEXT]);
    for (size_t i = 0; i < (size_t)s.width * s.height; i++)
        s.pixels[i] = s.pixels[i] == marker ? 0 : (s.pixels[i] & 0x00ffffff) | 0xe0000000u;
    *lw = w;
    *lh = h;
    return s;
}

int widget_drag_start(struct widget *w, const struct gui_drag_item *items, int nitems, int actions,
                      const struct image *icon, const char *label)
{
    struct window_state *ws = w->window ? window_state_of(w->window) : NULL;
    if (!ws || !ws->win)
        return -EINVAL;
    int iw = 0, ih = 0;
    struct surface img = icon || (label && *label) ? drag_image(w->window, icon, label, &iw, &ih)
                                                   : (struct surface){ 0 };
    /* The image lies below and to the right of the cursor's tip. */
    int r = gui_drag_start(ws->win, items, nitems, actions, img.pixels ? &img : NULL, -12, -12);
    free(img.pixels);
    if (r == 0) {
        ws->drag_source = w;
        ws->capture = NULL;
    }
    return r;
}

/* Deliver a drag event from w up through its ancestors; returns the
 * widget that took it. */
static struct widget *drag_dispatch(struct widget *w, struct event *e)
{
    for (struct widget *t = w; t; t = t->parent) {
        if (!t->enabled)
            return NULL;
        if (t->cls->event && t->cls->event(t, e))
            return t;
        e->x += t->x;
        e->y += t->y;
    }
    return NULL;
}

static void drag_leave(struct window_state *ws)
{
    struct widget *t = ws->drop_target;
    ws->drop_target = NULL;
    if (t && t->cls->event) {
        struct drag_event d = { 0 };
        struct event e = { .type = EV_DRAG_LEAVE, .drag = &d };
        t->cls->event(t, &e);
    }
}

static void drag_message(struct widget *window, struct wmsg *m)
{
    struct window_state *ws = window_state_of(window);
    struct drag_event d = { .actions = m->d, .action = m->c };
    struct event e = { .drag = &d };
    if (m->type == WM_DRAG_LEAVE) {
        drag_leave(ws);
        return;
    }
    if (m->type == WM_DRAG_END) {
        struct widget *src = ws->drag_source;
        ws->drag_source = NULL;
        ws->capture = NULL;
        d.action = m->a;
        e.type = EV_DRAG_END;
        if (src && src->cls->event)
            src->cls->event(src, &e);
        return;
    }
    if (m->window != ws->win->id)
        return;                         /* drops on the popup surface are refused */
    if (m->type == WM_DROP) {
        struct widget *t = ws->drop_target;
        if (t && t->cls->event) {
            int ax, ay;
            widget_abs(t, &ax, &ay);
            e.type = EV_DROP;
            e.x = m->a - ax;
            e.y = m->b - ay;
            d.data = gui_drop_data(&d.len, &d.mime);
            t->cls->event(t, &e);
        }
        drag_leave(ws);
        return;
    }
    struct widget *hit = widget_at(window, m->a, m->b);
    struct widget *taker = NULL;
    if (hit) {
        int ax, ay;
        widget_abs(hit, &ax, &ay);
        e.type = EV_DRAG_MOVE;
        e.x = m->a - ax;
        e.y = m->b - ay;
        taker = drag_dispatch(hit, &e);
    }
    if (ws->drop_target && ws->drop_target != taker)
        drag_leave(ws);
    ws->drop_target = taker;
    gui_drag_accept(taker ? d.accept_mime : NULL, d.accept_actions, d.preferred);
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

/* Accelerators of items in closed menus remain active; their mnemonics
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
    if (m->b) {
        tip_hide(window);
        caret_input(window);
    }
    if (m->b && m->a == KEY_ESC && ws->popup) {
        window_popup_close(window);
        return;
    }
    /* Typing into a text widget does not mark the focus; Tab and every
     * key of another widget do. */
    if (m->b && (m->a == KEY_TAB || !(ws->focus && ws->focus->accepts_text)))
        set_focus_visible(window, 1);
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
    caret_input(window);
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
    case WM_DRAG_ENTER:
    case WM_DRAG_MOTION:
    case WM_DRAG_LEAVE:
    case WM_DROP:
    case WM_DRAG_END:
        drag_message(window, m);
        break;
    case WM_FOCUS: {
        struct sig_change c = { m->a, NULL };
        widget_emit(window, "focus", &c);
        break;
    }
    case WM_HEADER:
        if (ws->header)
            widget_invalidate(ws->header);
        break;
    case WM_RESIZED: {
        if (ws->popup_win && m->window == ws->popup_win->id) {
            ws->popup->w = m->a;
            ws->popup->h = m->b;
            widget_relayout(ws->popup);
            break;
        }
        window_relayout_all(window);
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
    if (ws->blink_timer && w->app)
        app_timer_remove(w->app, ws->blink_timer);
    if (ws->popup_win)
        gui_destroy_window(ws->popup_win);
    if (ws->win)
        gui_destroy_window(ws->win);
    ws->win = NULL;
}

const struct widget_class window_class = {
    "window", sizeof(struct window), window_measure, window_layout, window_paint_bg, NULL, window_destroy, NULL,
};
