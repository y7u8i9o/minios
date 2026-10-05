/* Shell roles: toplevels with the configure and ack cycle, popups with
 * positioners and grabs, layer surfaces with exclusive zones,
 * decorations negotiation and the toplevel manager for the panel. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "comp.h"

static struct wire_server *server;
static int next_stack = 1, next_number = 1;
static struct positioner positioners[32];

/* ---- helpers ---- */

struct rect surface_rect(const struct csurface *s)
{
    struct rect r = { s->x, s->y, s->width, s->height };
    return r;
}

struct rect shell_desktop(void)
{
    struct rect d = { 0, 0, screen_w, screen_h };
    for (struct csurface *s = surface_first(); s; s = s->next) {
        if (s->role != ROLE_LAYER || !s->mapped || s->layer->exclusive <= 0)
            continue;
        int z = s->layer->exclusive;
        if (s->layer->anchor & LAYER_ANCHOR_BOTTOM) d.h -= z;
        else if (s->layer->anchor & LAYER_ANCHOR_TOP) { d.y += z; d.h -= z; }
        else if (s->layer->anchor & LAYER_ANCHOR_LEFT) { d.x += z; d.w -= z; }
        else if (s->layer->anchor & LAYER_ANCHOR_RIGHT) d.w -= z;
    }
    return d;
}

struct rect toplevel_frame(const struct toplevel *t)
{
    if (decor_has(t->s))
        return decor_frame(t->s);
    if (t->geo_set)
        return (struct rect){ t->s->x + t->geo.x, t->s->y + t->geo.y, t->geo.w, t->geo.h };
    return surface_rect(t->s);
}

void toplevel_configure_size(const struct toplevel *t, int *w, int *h)
{
    if (!decor_has(t->s) && t->geo_set) {
        *w = t->geo.w;
        *h = t->geo.h;
    } else {
        *w = t->s->width;
        *h = t->s->height;
    }
}

/* Leave 40 pixels of the frame on screen horizontally and its top row
 * (the title bar) inside the desktop area. */
static void clamp_toplevel(struct csurface *s)
{
    struct rect d = shell_desktop();
    struct rect f = toplevel_frame(s->toplevel);
    if (f.x + f.w < d.x + 40) s->x += d.x + 40 - (f.x + f.w);
    if (f.x > d.x + d.w - 40) s->x -= f.x - (d.x + d.w - 40);
    if (f.y < d.y) s->y += d.y - f.y;
    if (f.y > d.y + d.h - TITLE_H) s->y -= f.y - (d.y + d.h - TITLE_H);
}

static void damage_surface(struct csurface *s)
{
    scene_damage(decor_has(s) ? decor_extent(s) : surface_rect(s));
}

/* ---- toplevel manager ---- */

static void send_handle_state(struct wire_resource *handle, struct toplevel *t)
{
    uint32_t states[3];
    int n = 0;
    if (t->maximized) states[n++] = STATE_MAXIMIZED;
    if (t->activated) states[n++] = STATE_ACTIVATED;
    if (t->minimized) states[n++] = STATE_MINIMIZED;
    struct wire_array a = { states, sizeof(uint32_t) * (size_t)n };
    toplevel_handle_send_state(handle, &a);
}

static void h_handle_activate(struct wire_client *c, struct wire_resource *self, struct wire_resource *seat)
{
    struct toplevel *t = self->data;
    if (t) {
        toplevel_set_minimized(t, 0);
        toplevel_activate(t);
    }
}
static void h_handle_minimize(struct wire_client *c, struct wire_resource *self) { if (self->data) toplevel_set_minimized(self->data, 1); }
static void h_handle_close(struct wire_client *c, struct wire_resource *self) { if (self->data) toplevel_close(self->data); }
static void h_handle_destroy(struct wire_client *c, struct wire_resource *self) { wire_resource_destroy(self); }
static const struct toplevel_handle_impl handle_handlers = { h_handle_activate, h_handle_minimize, h_handle_close, h_handle_destroy };

static void handle_gone(struct wire_resource *r)
{
    struct toplevel *t = r->data;
    if (t)
        t->handle_count--;
}

/* Every manager gets a handle for the toplevel; a change updates all
 * handles that refer to it (found by their data pointer). */
static void manager_announce(struct wire_resource *manager, struct toplevel *t)
{
    struct wire_resource *h = wire_resource_create(manager->client, &toplevel_handle_interface, 1, 0);
    if (!h)
        return;
    wire_resource_set_listener(h, &handle_handlers, t, handle_gone);
    t->handle_count++;
    toplevel_manager_send_toplevel(manager, h);
    toplevel_handle_send_title(h, t->title);
    toplevel_handle_send_app_id(h, t->app_id);
    send_handle_state(h, t);
}

static void for_each_handle(struct toplevel *t, void (*fn)(struct wire_resource *, struct toplevel *))
{
    for (struct wire_client *c = wire_server_first_client(server); c; c = wire_client_next(c)) {
        struct client *cl = wire_client_get_user_data(c);
        if (!cl || !cl->manager)
            continue;
        for (struct wire_resource *r = wire_client_first_resource(c); r; r = r->next)
            if (r->obj.interface == &toplevel_handle_interface && r->data == t)
                fn(r, t);
    }
}

static void handle_title(struct wire_resource *h, struct toplevel *t) { toplevel_handle_send_title(h, t->title); }
static void handle_app_id(struct wire_resource *h, struct toplevel *t) { toplevel_handle_send_app_id(h, t->app_id); }
static void handle_closed(struct wire_resource *h, struct toplevel *t) { toplevel_handle_send_closed(h); h->data = NULL; }

static void announce_to_all(struct toplevel *t)
{
    for (struct wire_client *c = wire_server_first_client(server); c; c = wire_client_next(c)) {
        struct client *cl = wire_client_get_user_data(c);
        if (cl && cl->manager)
            manager_announce(cl->manager, t);
    }
}

static void h_manager_destroy(struct wire_client *c, struct wire_resource *self) { wire_resource_destroy(self); }
static const struct toplevel_manager_impl manager_handlers = { h_manager_destroy };

static void manager_gone(struct wire_resource *r)
{
    struct client *cl = wire_client_get_user_data(r->client);
    if (cl && cl->manager == r)
        cl->manager = NULL;
}

static void bind_manager(struct wire_client *c, void *data, uint32_t version, uint32_t id)
{
    struct wire_resource *r = wire_resource_create(c, &toplevel_manager_interface, (int)version, id);
    if (!r)
        return;
    wire_resource_set_listener(r, &manager_handlers, NULL, manager_gone);
    struct client *cl = wire_client_get_user_data(c);
    if (cl)
        cl->manager = r;
    for (struct csurface *s = surface_first(); s; s = s->next)
        if (s->role == ROLE_TOPLEVEL)
            manager_announce(r, s->toplevel);
}

/* ---- toplevels ---- */

static void send_configure(struct toplevel *t, int w, int h)
{
    uint32_t states[2];
    int n = 0;
    if (t->maximized) states[n++] = STATE_MAXIMIZED;
    if (t->activated) states[n++] = STATE_ACTIVATED;
    struct wire_array a = { states, sizeof(uint32_t) * (size_t)n };
    t->configure_serial = comp_serial();
    t->pending_w = w;
    t->pending_h = h;
    toplevel_send_configure(t->res, t->configure_serial, w, h, &a);
    comp_log("toplevel %d configured %dx%d serial %u", t->number, w, h, t->configure_serial);
}

void toplevel_configure(struct toplevel *t, int w, int h)
{
    if (t->min_w && w < t->min_w) w = t->min_w;
    if (t->min_h && h < t->min_h) h = t->min_h;
    if (t->max_w && w > t->max_w) w = t->max_w;
    if (t->max_h && h > t->max_h) h = t->max_h;
    send_configure(t, w, h);
}

struct toplevel *toplevel_focused(void)
{
    struct toplevel *best = NULL;
    for (struct csurface *s = surface_first(); s; s = s->next)
        if (s->role == ROLE_TOPLEVEL && s->toplevel->activated)
            best = s->toplevel;
    return best;
}

static struct toplevel *modal_child(struct toplevel *parent)
{
    struct toplevel *best = NULL;
    for (struct csurface *s = surface_first(); s; s = s->next)
        if (s->role == ROLE_TOPLEVEL && s->mapped && !s->toplevel->minimized && s->toplevel->modal &&
            s->toplevel->parent == parent && (!best || s->stack > best->s->stack))
            best = s->toplevel;
    return best;
}

int toplevel_blocked(struct toplevel *t)
{
    struct toplevel *modal = modal_child(t);
    if (!modal)
        return 0;
    toplevel_activate(modal);
    return 1;
}

void toplevel_activate(struct toplevel *t)
{
    struct toplevel *modal = modal_child(t);
    if (modal)
        t = modal;
    struct toplevel *old = toplevel_focused();
    if (old == t && t->s->stack == next_stack - 1)
        return;
    int w, h;
    if (old && old != t) {
        old->activated = 0;
        damage_surface(old->s);
        toplevel_configure_size(old, &w, &h);
        send_configure(old, w, h);
        for_each_handle(old, send_handle_state);
    }
    t->activated = 1;
    t->s->stack = next_stack++;
    damage_surface(t->s);
    toplevel_configure_size(t, &w, &h);
    send_configure(t, w, h);
    for_each_handle(t, send_handle_state);
    seat_set_keyboard_focus(t->s);
    comp_log("toplevel %d activated", t->number);
}

void toplevel_set_maximized(struct toplevel *t, int on)
{
    if (t->maximized == !!on)
        return;
    struct rect d = shell_desktop();
    damage_surface(t->s);
    if (on) {
        t->saved_x = t->s->x;
        t->saved_y = t->s->y;
        toplevel_configure_size(t, &t->saved_w, &t->saved_h);
        t->maximized = 1;
        int top = decor_has(t->s) ? TITLE_H + BORDER : 0;
        int bw = decor_has(t->s) ? BORDER : 0;
        t->s->x = d.x + bw;
        t->s->y = d.y + top;
        send_configure(t, d.w - 2 * bw, d.h - top - bw);
        comp_log("toplevel %d maximized", t->number);
    } else {
        t->maximized = 0;
        t->s->x = t->saved_x;
        t->s->y = t->saved_y;
        send_configure(t, t->saved_w, t->saved_h);
        comp_log("toplevel %d restored", t->number);
    }
    damage_surface(t->s);
    for_each_handle(t, send_handle_state);
}

void toplevel_set_minimized(struct toplevel *t, int on)
{
    if (t->minimized == !!on)
        return;
    t->minimized = !!on;
    damage_surface(t->s);
    comp_log("toplevel %d %s", t->number, on ? "minimized" : "shown");
    if (on && t->activated) {
        t->activated = 0;
        int w, h;
        toplevel_configure_size(t, &w, &h);
        send_configure(t, w, h);
        struct toplevel *next = NULL;
        for (struct csurface *s = surface_first(); s; s = s->next)
            if (s->role == ROLE_TOPLEVEL && !s->toplevel->minimized && s->mapped && (!next || s->stack > next->s->stack))
                next = s->toplevel;
        seat_set_keyboard_focus(NULL);
        if (next)
            toplevel_activate(next);
    } else if (!on) {
        toplevel_activate(t);
    }
    for_each_handle(t, send_handle_state);
}

void toplevel_close(struct toplevel *t)
{
    struct toplevel *modal = modal_child(t);
    if (modal)
        t = modal;
    comp_log("toplevel %d close requested", t->number);
    toplevel_send_close(t->res);
}

void toplevel_move(struct toplevel *t, int x, int y)
{
    damage_surface(t->s);
    t->s->x = x;
    t->s->y = y;
    clamp_toplevel(t->s);
    damage_surface(t->s);
}

void toplevel_cycle(void)
{
    struct toplevel *lowest = NULL;
    int visible = 0;
    for (struct csurface *s = surface_first(); s; s = s->next) {
        if (s->role != ROLE_TOPLEVEL || s->toplevel->minimized || !s->mapped)
            continue;
        visible++;
        if (!lowest || s->stack < lowest->s->stack)
            lowest = s->toplevel;
    }
    comp_log("alt-tab");
    if (visible >= 2)
        toplevel_activate(lowest);
}

static void h_set_title(struct wire_client *c, struct wire_resource *self, const char *title)
{
    struct toplevel *t = self->data;
    strlcpy(t->title, title, sizeof t->title);
    damage_surface(t->s);
    for_each_handle(t, handle_title);
}
static void h_set_app_id(struct wire_client *c, struct wire_resource *self, const char *app_id)
{
    struct toplevel *t = self->data;
    strlcpy(t->app_id, app_id, sizeof t->app_id);
    for_each_handle(t, handle_app_id);
}
static void h_set_min_size(struct wire_client *c, struct wire_resource *self, int32_t w, int32_t h)
{ struct toplevel *t = self->data; t->min_w = w; t->min_h = h; }
static void h_set_max_size(struct wire_client *c, struct wire_resource *self, int32_t w, int32_t h)
{ struct toplevel *t = self->data; t->max_w = w; t->max_h = h; }
static void h_move(struct wire_client *c, struct wire_resource *self, struct wire_resource *seat, uint32_t serial)
{
    struct toplevel *t = self->data;
    /* The button must still be pressed: a request that arrives after the
     * release (a client that answered late) would drag with no button. */
    if (seat_validate_grab(t->s->client, t->s, serial) && !t->maximized && seat_buttons())
        decor_press(t->s, 1 | 0x100);           /* a move grab from the client */
}
static void h_resize(struct wire_client *c, struct wire_resource *self, struct wire_resource *seat, uint32_t serial, uint32_t edges)
{
    struct toplevel *t = self->data;
    if (seat_validate_grab(t->s->client, t->s, serial) && !t->maximized && seat_buttons())
        decor_press(t->s, 1 | 0x200 | (int)(edges << 16));
}
static void h_set_maximized(struct wire_client *c, struct wire_resource *self) { toplevel_set_maximized(self->data, 1); }
static void h_unset_maximized(struct wire_client *c, struct wire_resource *self) { toplevel_set_maximized(self->data, 0); }
static void h_set_minimized(struct wire_client *c, struct wire_resource *self) { toplevel_set_minimized(self->data, 1); }
static void h_ack_configure(struct wire_client *c, struct wire_resource *self, uint32_t serial)
{
    struct toplevel *t = self->data;
    if (!t->configure_serial || serial != t->configure_serial) {
        wire_client_post_error(c, self, 21, "invalid configure serial");
        return;
    }
    t->acked_serial = serial;
}
static void h_toplevel_destroy(struct wire_client *c, struct wire_resource *self) { wire_resource_destroy(self); }
static void h_set_parent(struct wire_client *c, struct wire_resource *self, struct wire_resource *parent)
{
    struct toplevel *t = self->data;
    struct toplevel *p = parent ? parent->data : NULL;
    if (p == t) {
        wire_client_post_error(c, self, 22, "toplevel cannot parent itself");
        return;
    }
    t->parent = p;
}
static void h_set_window_geometry(struct wire_client *c, struct wire_resource *self, int32_t x, int32_t y, int32_t w, int32_t h)
{
    struct toplevel *t = self->data;
    if (w <= 0 || h <= 0) {
        t->geo_set = 0;
        return;
    }
    t->geo = (struct rect){ x, y, w, h };
    t->geo_set = 1;
}
static void h_set_modal(struct wire_client *c, struct wire_resource *self, uint32_t modal)
{
    struct toplevel *t = self->data;
    t->modal = modal != 0;
}
static const struct toplevel_impl toplevel_handlers = {
    h_set_title, h_set_app_id, h_set_min_size, h_set_max_size, h_move, h_resize, h_set_maximized,
    h_unset_maximized, h_set_minimized, h_ack_configure, h_toplevel_destroy, h_set_parent, h_set_modal,
    h_set_window_geometry,
};

static void toplevel_gone(struct wire_resource *r)
{
    struct toplevel *t = r->data;
    comp_log("toplevel %d destroyed", t->number);
    for_each_handle(t, handle_closed);
    if (t->s->mapped)
        damage_surface(t->s);
    int was_active = t->activated;
    t->s->role = ROLE_NONE;
    t->s->toplevel = NULL;
    t->s->mapped = 0;
    seat_surface_gone(t->s);
    if (t->decoration)
        t->decoration->data = NULL;
    for (struct csurface *s = surface_first(); s; s = s->next)
        if (s->role == ROLE_TOPLEVEL && s->toplevel && s->toplevel->parent == t)
            s->toplevel->parent = NULL;
    free(t);
    if (was_active) {
        struct toplevel *next = NULL;
        for (struct csurface *s = surface_first(); s; s = s->next)
            if (s->role == ROLE_TOPLEVEL && !s->toplevel->minimized && s->mapped && (!next || s->stack > next->s->stack))
                next = s->toplevel;
        if (next)
            toplevel_activate(next);
    }
}

static void h_get_toplevel(struct wire_client *c, struct wire_resource *self, uint32_t id, struct wire_resource *surface)
{
    struct csurface *s = surface->data;
    if (s->role != ROLE_NONE) {
        wire_client_post_error(c, self, 20, "surface already has a role");
        return;
    }
    struct toplevel *t = calloc(1, sizeof *t);
    struct wire_resource *r = t ? wire_resource_create(c, &toplevel_interface, 1, id) : NULL;
    if (!r) {
        free(t);
        return;
    }
    t->s = s;
    t->res = r;
    t->number = next_number++;
    t->decor_mode = settings.decor_default;
    strcpy(t->title, "untitled");
    s->role = ROLE_TOPLEVEL;
    s->toplevel = t;
    s->stack = next_stack++;
    wire_resource_set_listener(r, &toplevel_handlers, t, toplevel_gone);
    send_configure(t, 0, 0);
    announce_to_all(t);
}

/* ---- decorations ---- */

static void h_decor_set_mode(struct wire_client *c, struct wire_resource *self, uint32_t mode)
{
    struct toplevel *t = self->data;
    if (!t)
        return;
    t->decor_mode = mode == DECOR_CLIENT ? DECOR_CLIENT : DECOR_SERVER;
    decoration_send_mode(self, (uint32_t)t->decor_mode);
    if (t->s->mapped)
        scene_damage_all();
    comp_log("toplevel %d decorations %s", t->number, t->decor_mode == DECOR_CLIENT ? "client" : "server");
}
static void h_decor_destroy(struct wire_client *c, struct wire_resource *self) { wire_resource_destroy(self); }
static const struct decoration_impl decoration_handlers = { h_decor_set_mode, h_decor_destroy };
static void decoration_gone(struct wire_resource *r)
{
    struct toplevel *t = r->data;
    if (t)
        t->decoration = NULL;
}

static void h_get_decoration(struct wire_client *c, struct wire_resource *self, uint32_t id, struct wire_resource *toplevel)
{
    struct toplevel *t = toplevel->data;
    struct wire_resource *r = wire_resource_create(c, &decoration_interface, 1, id);
    if (!r)
        return;
    t->decoration = r;
    wire_resource_set_listener(r, &decoration_handlers, t, decoration_gone);
    decoration_send_mode(r, (uint32_t)t->decor_mode);
}

/* ---- positioners and popups ---- */

static void h_pos_set_size(struct wire_client *c, struct wire_resource *self, int32_t w, int32_t h)
{ struct positioner *p = self->data; p->w = w; p->h = h; }
static void h_pos_set_anchor_rect(struct wire_client *c, struct wire_resource *self, int32_t x, int32_t y, int32_t w, int32_t h)
{ struct positioner *p = self->data; p->ax = x; p->ay = y; p->aw = w; p->ah = h; }
static void h_pos_set_anchor(struct wire_client *c, struct wire_resource *self, uint32_t a) { ((struct positioner *)self->data)->anchor = (int)a; }
static void h_pos_set_gravity(struct wire_client *c, struct wire_resource *self, uint32_t g) { ((struct positioner *)self->data)->gravity = (int)g; }
static void h_pos_set_offset(struct wire_client *c, struct wire_resource *self, int32_t x, int32_t y)
{ struct positioner *p = self->data; p->ox = x; p->oy = y; }
static void h_pos_set_adjust(struct wire_client *c, struct wire_resource *self, uint32_t a) { ((struct positioner *)self->data)->adjust = (int)a; }
static void h_pos_destroy(struct wire_client *c, struct wire_resource *self) { wire_resource_destroy(self); }
static const struct positioner_impl positioner_handlers = {
    h_pos_set_size, h_pos_set_anchor_rect, h_pos_set_anchor, h_pos_set_gravity, h_pos_set_offset, h_pos_set_adjust, h_pos_destroy,
};
static void positioner_gone(struct wire_resource *r) { ((struct positioner *)r->data)->res = NULL; }

static void h_create_positioner(struct wire_client *c, struct wire_resource *self, uint32_t id)
{
    struct positioner *p = NULL;
    for (int i = 0; i < 32; i++)
        if (!positioners[i].res) {
            p = &positioners[i];
            break;
        }
    struct wire_resource *r = p ? wire_resource_create(c, &positioner_interface, 1, id) : NULL;
    if (!r)
        return;
    memset(p, 0, sizeof *p);
    p->res = r;
    wire_resource_set_listener(r, &positioner_handlers, p, positioner_gone);
}

static int flip_x(int a)
{
    static const int map[] = { 0, ANCHOR_TOP, ANCHOR_BOTTOM, ANCHOR_RIGHT, ANCHOR_LEFT,
        ANCHOR_TOP_RIGHT, ANCHOR_BOTTOM_RIGHT, ANCHOR_TOP_LEFT, ANCHOR_BOTTOM_LEFT };
    return a >= 0 && a <= ANCHOR_BOTTOM_RIGHT ? map[a] : a;
}

static int flip_y(int a)
{
    static const int map[] = { 0, ANCHOR_BOTTOM, ANCHOR_TOP, ANCHOR_LEFT, ANCHOR_RIGHT,
        ANCHOR_BOTTOM_LEFT, ANCHOR_TOP_LEFT, ANCHOR_BOTTOM_RIGHT, ANCHOR_TOP_RIGHT };
    return a >= 0 && a <= ANCHOR_BOTTOM_RIGHT ? map[a] : a;
}

static void position_raw(const struct positioner *p, int anchor, int gravity, int ox, int oy,
                         int w, int h, int *x, int *y)
{
    int ax = p->ax, ay = p->ay;
    switch (anchor) {
    case ANCHOR_TOP: ax += p->aw / 2; break;
    case ANCHOR_BOTTOM: ax += p->aw / 2; ay += p->ah; break;
    case ANCHOR_LEFT: ay += p->ah / 2; break;
    case ANCHOR_RIGHT: ax += p->aw; ay += p->ah / 2; break;
    case ANCHOR_TOP_LEFT: break;
    case ANCHOR_BOTTOM_LEFT: ay += p->ah; break;
    case ANCHOR_TOP_RIGHT: ax += p->aw; break;
    case ANCHOR_BOTTOM_RIGHT: ax += p->aw; ay += p->ah; break;
    default: ax += p->aw / 2; ay += p->ah / 2; break;
    }
    int px = ax, py = ay;
    switch (gravity) {
    case ANCHOR_TOP: px -= w / 2; py -= h; break;
    case ANCHOR_BOTTOM: px -= w / 2; break;
    case ANCHOR_LEFT: px -= w; py -= h / 2; break;
    case ANCHOR_RIGHT: py -= h / 2; break;
    case ANCHOR_TOP_LEFT: px -= w; py -= h; break;
    case ANCHOR_BOTTOM_LEFT: px -= w; break;
    case ANCHOR_TOP_RIGHT: py -= h; break;
    case ANCHOR_BOTTOM_RIGHT: break;
    default: px -= w / 2; py -= h / 2; break;
    }
    *x = px + ox;
    *y = py + oy;
}

static int overflow(int p, int size, int limit)
{
    int n = p < 0 ? -p : 0;
    if (p + size > limit)
        n += p + size - limit;
    return n;
}

/* Apply the xdg-style flip/slide/resize constraint sequence. The
 * resulting coordinates are relative to the parent and the size can
 * be reduced when the requested popup is larger than the output. */
static void positioner_place(const struct positioner *p, int parent_x, int parent_y,
                             int *w, int *h, int *x, int *y)
{
    int px, py;
    position_raw(p, p->anchor, p->gravity, p->ox, p->oy, *w, *h, &px, &py);
    int gx = parent_x + px, gy = parent_y + py;
    if ((p->adjust & ADJUST_FLIP_X) && overflow(gx, *w, screen_w)) {
        int tx, ty;
        position_raw(p, flip_x(p->anchor), flip_x(p->gravity), -p->ox, p->oy, *w, *h, &tx, &ty);
        if (overflow(parent_x + tx, *w, screen_w) < overflow(gx, *w, screen_w))
            gx = parent_x + tx;
    }
    if ((p->adjust & ADJUST_FLIP_Y) && overflow(gy, *h, screen_h)) {
        int tx, ty;
        position_raw(p, flip_y(p->anchor), flip_y(p->gravity), p->ox, -p->oy, *w, *h, &tx, &ty);
        if (overflow(parent_y + ty, *h, screen_h) < overflow(gy, *h, screen_h))
            gy = parent_y + ty;
    }
    if (p->adjust & ADJUST_SLIDE_X) {
        if (gx + *w > screen_w) gx = screen_w - *w;
        if (gx < 0) gx = 0;
    }
    if (p->adjust & ADJUST_SLIDE_Y) {
        if (gy + *h > screen_h) gy = screen_h - *h;
        if (gy < 0) gy = 0;
    }
    if ((p->adjust & ADJUST_RESIZE_X) && overflow(gx, *w, screen_w)) {
        if (gx < 0) gx = 0;
        *w = screen_w - gx;
        if (*w < 1) *w = 1;
    }
    if ((p->adjust & ADJUST_RESIZE_Y) && overflow(gy, *h, screen_h)) {
        if (gy < 0) gy = 0;
        *h = screen_h - gy;
        if (*h < 1) *h = 1;
    }
    *x = gx - parent_x;
    *y = gy - parent_y;
}

static void popup_configure(struct popup *p)
{
    int w = p->pos.w > 0 ? p->pos.w : p->s->width, h = p->pos.h > 0 ? p->pos.h : p->s->height;
    positioner_place(&p->pos, p->parent->x, p->parent->y, &w, &h, &p->x, &p->y);
    p->serial = comp_serial();
    p->pending_w = w;
    p->pending_h = h;
    popup_send_configure(p->res, p->serial, p->x, p->y, w, h);
}

static void h_popup_grab(struct wire_client *c, struct wire_resource *self, struct wire_resource *seat, uint32_t serial)
{
    struct popup *p = self->data;
    if (!seat_validate_grab(p->s->client, p->parent, serial))
        return;
    p->grab = 1;
    seat_set_keyboard_focus(p->s);
}
static void h_popup_ack(struct wire_client *c, struct wire_resource *self, uint32_t serial)
{
    struct popup *p = self->data;
    if (!p->serial || serial != p->serial) {
        wire_client_post_error(c, self, 21, "invalid popup configure serial");
        return;
    }
    p->acked_serial = serial;
}
static void h_popup_destroy(struct wire_client *c, struct wire_resource *self) { wire_resource_destroy(self); }
static const struct popup_impl popup_handlers = { h_popup_grab, h_popup_ack, h_popup_destroy };

static void popup_gone(struct wire_resource *r)
{
    struct popup *p = r->data;
    if (p->s->mapped)
        scene_damage(surface_rect(p->s));
    comp_log("popup %d destroyed", p->s->id);
    p->s->role = ROLE_NONE;
    p->s->popup = NULL;
    p->s->mapped = 0;
    seat_surface_gone(p->s);
    if (seat_keyboard_focus() == NULL) {
        struct toplevel *t = toplevel_focused();
        if (t)
            seat_set_keyboard_focus(t->s);
    }
    free(p);
}

static void h_get_popup(struct wire_client *c, struct wire_resource *self, uint32_t id, struct wire_resource *surface,
                        struct wire_resource *parent, struct wire_resource *positioner)
{
    struct csurface *s = surface->data, *ps = parent->data;
    if (s->role != ROLE_NONE) {
        wire_client_post_error(c, self, 20, "surface already has a role");
        return;
    }
    struct popup *p = calloc(1, sizeof *p);
    struct wire_resource *r = p ? wire_resource_create(c, &popup_interface, 1, id) : NULL;
    if (!r) {
        free(p);
        return;
    }
    p->s = s;
    p->parent = ps;
    p->res = r;
    p->pos = *(struct positioner *)positioner->data;
    s->role = ROLE_POPUP;
    s->popup = p;
    wire_resource_set_listener(r, &popup_handlers, p, popup_gone);
    popup_configure(p);
}

void popup_dismiss_all(void)
{
    for (struct csurface *s = surface_first(); s; s = s->next)
        if (s->role == ROLE_POPUP && s->mapped) {
            comp_log("popup %d dismissed", s->id);
            popup_send_done(s->popup->res);
            s->mapped = 0;
            scene_damage(surface_rect(s));
        }
}

struct csurface *popup_grab_surface(void)
{
    struct csurface *best = NULL;
    for (struct csurface *s = surface_first(); s; s = s->next)
        if (s->role == ROLE_POPUP && s->mapped && s->popup->grab)
            best = s;
    return best;
}

/* ---- layer surfaces ---- */

static void layer_configure(struct layer *l)
{
    /* A dimension of 0 takes the area left by exclusive zones (the
     * desktop background under the panel). */
    struct rect d = shell_desktop();
    int w = l->w > 0 ? l->w : d.w, h = l->h > 0 ? l->h : d.h;
    l->serial = comp_serial();
    l->pending_w = w;
    l->pending_h = h;
    if (l->nsent == 4) {
        memmove(l->sent, l->sent + 1, 3 * sizeof l->sent[0]);
        l->nsent = 3;
    }
    l->sent[l->nsent].serial = l->serial;
    l->sent[l->nsent].w = w;
    l->sent[l->nsent].h = h;
    l->nsent++;
    layer_surface_send_configure(l->res, l->serial, w, h);
}

static void h_layer_set_anchor(struct wire_client *c, struct wire_resource *self, uint32_t a) { ((struct layer *)self->data)->anchor = (int)a; }
static void h_layer_set_zone(struct wire_client *c, struct wire_resource *self, int32_t z) { ((struct layer *)self->data)->exclusive = z; }
static void h_layer_set_size(struct wire_client *c, struct wire_resource *self, int32_t w, int32_t h)
{ struct layer *l = self->data; l->w = w; l->h = h; layer_configure(l); }
static void h_layer_set_kbd(struct wire_client *c, struct wire_resource *self, uint32_t m) { ((struct layer *)self->data)->interactive = (int)m; }
static void h_layer_ack(struct wire_client *c, struct wire_resource *self, uint32_t serial)
{
    struct layer *l = self->data;
    int i = 0;
    while (i < l->nsent && l->sent[i].serial != serial)
        i++;
    if (!l->serial || i == l->nsent) {
        wire_client_post_error(c, self, 21, "invalid layer configure serial");
        return;
    }
    l->acked_serial = serial;
    l->acked_w = l->sent[i].w;
    l->acked_h = l->sent[i].h;
    /* The acknowledged configure supersedes the older ones. */
    memmove(l->sent, l->sent + i + 1, (size_t)(l->nsent - i - 1) * sizeof l->sent[0]);
    l->nsent -= i + 1;
}
static void h_layer_destroy(struct wire_client *c, struct wire_resource *self) { wire_resource_destroy(self); }
static void h_layer_set_margin(struct wire_client *c, struct wire_resource *self, int32_t top, int32_t right,
                               int32_t bottom, int32_t left)
{
    struct layer *l = self->data;
    l->margin[0] = top;
    l->margin[1] = right;
    l->margin[2] = bottom;
    l->margin[3] = left;
    l->has_margin = 1;
}
static const struct layer_surface_impl layer_handlers = {
    h_layer_set_anchor, h_layer_set_zone, h_layer_set_size, h_layer_set_kbd, h_layer_ack, h_layer_destroy,
    h_layer_set_margin,
};

/* Position a layer surface by its anchor. A surface anchored on one side
 * touches the screen edge, and a surface anchored on two opposite sides
 * starts at the edge of the desktop area. When margins are set, each
 * anchored edge is placed at its margin from the edge of the desktop area
 * instead. A surface without an anchor on an axis is centred on the
 * screen on that axis. */
static void layer_place(struct csurface *s)
{
    struct layer *l = s->layer;
    struct rect d = shell_desktop();
    int lr = l->anchor & (LAYER_ANCHOR_LEFT | LAYER_ANCHOR_RIGHT);
    int tb = l->anchor & (LAYER_ANCHOR_TOP | LAYER_ANCHOR_BOTTOM);
    int cx = (screen_w - s->width) / 2, cy = (screen_h - s->height) / 2;
    if (l->has_margin) {
        s->x = lr == LAYER_ANCHOR_RIGHT ? d.x + d.w - s->width - l->margin[1] : lr ? d.x + l->margin[3] : cx;
        s->y = tb == LAYER_ANCHOR_BOTTOM ? d.y + d.h - s->height - l->margin[2] : tb ? d.y + l->margin[0] : cy;
        return;
    }
    s->x = lr == LAYER_ANCHOR_RIGHT ? screen_w - s->width : lr == (LAYER_ANCHOR_LEFT | LAYER_ANCHOR_RIGHT) ? d.x
         : lr ? 0 : cx;
    s->y = tb == LAYER_ANCHOR_BOTTOM ? screen_h - s->height : tb == (LAYER_ANCHOR_TOP | LAYER_ANCHOR_BOTTOM) ? d.y
         : tb ? 0 : cy;
}

static void layer_gone(struct wire_resource *r)
{
    struct layer *l = r->data;
    if (l->s->mapped)
        scene_damage(surface_rect(l->s));
    int focused = seat_keyboard_focus() == l->s;
    l->s->role = ROLE_NONE;
    l->s->layer = NULL;
    l->s->mapped = 0;
    seat_surface_gone(l->s);
    /* An overlay that had the keyboard returns it to the active window. */
    struct toplevel *t = toplevel_focused();
    if (focused && t && t->s)
        seat_set_keyboard_focus(t->s);
    free(l);
}

static void h_get_layer_surface(struct wire_client *c, struct wire_resource *self, uint32_t id, struct wire_resource *surface,
                                uint32_t layer_n, const char *ns)
{
    struct csurface *s = surface->data;
    if (s->role != ROLE_NONE) {
        wire_client_post_error(c, self, 20, "surface already has a role");
        return;
    }
    struct layer *l = calloc(1, sizeof *l);
    struct wire_resource *r = l ? wire_resource_create(c, &layer_surface_interface, 1, id) : NULL;
    if (!r) {
        free(l);
        return;
    }
    l->s = s;
    l->res = r;
    l->layer = layer_n;
    s->role = ROLE_LAYER;
    s->layer = l;
    wire_resource_set_listener(r, &layer_handlers, l, layer_gone);
    comp_log("layer surface %d (%s)", s->id, ns);
}

static void h_pong(struct wire_client *c, struct wire_resource *self, uint32_t serial)
{
    struct client *cl = wire_client_get_user_data(c);
    if (cl && serial == cl->ping_serial)
        cl->last_pong = uptime_ms();
}

static void h_set_pid(struct wire_client *c, struct wire_resource *self, uint32_t pid)
{
    struct client *cl = wire_client_get_user_data(c);
    if (cl) {
        cl->pid = (int)pid;
        trace_client(cl, 1);
    }
}

static const struct shell_impl shell_handlers = { h_get_toplevel, h_get_popup, h_create_positioner, h_get_layer_surface,
                                                  h_get_decoration, h_pong, h_set_pid };

static void bind_shell(struct wire_client *c, void *data, uint32_t version, uint32_t id)
{
    struct wire_resource *r = wire_resource_create(c, &shell_interface, (int)version, id);
    if (r)
        wire_resource_set_listener(r, &shell_handlers, NULL, NULL);
    struct client *cl = wire_client_get_user_data(c);
    if (cl)
        cl->shell_res = r;
}

/* ---- commits ---- */

static void logical_buffer_size(const struct csurface *s, const struct buffer *b, int *w, int *h)
{
    int scale = s->pending.state_set ? s->pending.scale : s->current.scale;
    int transform = s->pending.state_set ? s->pending.transform : s->current.transform;
    if (scale < 1)
        scale = 1;
    int bw = b->width / scale, bh = b->height / scale;
    *w = transform & 1 ? bh : bw;
    *h = transform & 1 ? bw : bh;
}

int surface_commit_allowed(struct wire_client *c, struct csurface *s, struct buffer *b)
{
    if (!b)
        return 1;
    uint32_t serial = 0, acked = 0;
    int want_w = 0, want_h = 0;
    switch (s->role) {
    case ROLE_TOPLEVEL:
        serial = s->toplevel->configure_serial;
        acked = s->toplevel->acked_serial;
        want_w = s->toplevel->pending_w;
        want_h = s->toplevel->pending_h;
        break;
    case ROLE_POPUP:
        serial = s->popup->serial;
        acked = s->popup->acked_serial;
        want_w = s->popup->pending_w;
        want_h = s->popup->pending_h;
        break;
    case ROLE_LAYER:
        serial = s->layer->serial;
        acked = s->layer->acked_serial;
        want_w = s->layer->pending_w;
        want_h = s->layer->pending_h;
        break;
    default:
        return 1;
    }
    if (serial && acked != serial) {
        /* A state-only configure does not make the already mapped
         * geometry unsafe. Continue accepting matching old-size buffers
         * until the client has received the event; do not consume the
         * configure. */
        if (s->mapped && (want_w <= 0 || want_w == s->width) &&
            (want_h <= 0 || want_h == s->height))
            return 1;
        if (s->mapped && !s->pending.has_buffer)
            return 1;
        /* A frame sent before the configure arrived: a buffer of the old
         * geometry, from either half of the client's buffer pair. */
        int bw, bh;
        logical_buffer_size(s, b, &bw, &bh);
        if (s->mapped && bw == s->width && bh == s->height)
            return 1;
        /* A layer whose client acknowledged an older configure commits a
         * buffer of that configure.  The newer one remains pending. */
        if (s->role == ROLE_LAYER && acked && bw == s->layer->acked_w && bh == s->layer->acked_h)
            return 1;
        wire_client_post_error(c, s->res, 23, "buffer committed before configure acknowledgement");
        return 0;
    }
    int w, h;
    logical_buffer_size(s, b, &w, &h);
    if (s->role == ROLE_TOPLEVEL && s->toplevel->geo_set) {
        /* The configure described the window geometry, sent before
         * this commit. */
        w = s->toplevel->geo.w;
        h = s->toplevel->geo.h;
    }
    if ((want_w > 0 && w != want_w) || (want_h > 0 && h != want_h)) {
        wire_client_post_error(c, s->res, 24, "buffer size does not match acknowledged configure");
        return 0;
    }
    if (s->role == ROLE_TOPLEVEL) {
        s->toplevel->configure_serial = s->toplevel->acked_serial = 0;
    } else if (s->role == ROLE_POPUP) {
        s->popup->serial = s->popup->acked_serial = 0;
    } else if (s->role == ROLE_LAYER) {
        s->layer->serial = s->layer->acked_serial = 0;
    }
    return 1;
}

void shell_surface_committed(struct csurface *s, int first_map)
{
    switch (s->role) {
    case ROLE_TOPLEVEL: {
        struct toplevel *t = s->toplevel;
        if (first_map && !t->placed) {
            /* Placement works on the visible frame: centred on the
             * parent, or cascading from (40, 30) of the desktop area,
             * which puts the contents under a 30 pixel toolkit header
             * bar at y 60. */
            t->placed = 1;
            struct rect d = shell_desktop();
            struct rect f = toplevel_frame(t);
            int fx, fy;
            if (t->parent && t->parent->s && t->parent->s->mapped) {
                struct rect pf = toplevel_frame(t->parent);
                fx = pf.x + (pf.w - f.w) / 2;
                fy = pf.y + (pf.h - f.h) / 2;
            } else {
                int n = (t->number - 1) % 8;
                fx = d.x + 40 + n * 30;
                fy = d.y + 30 + n * 30;
            }
            if (fx + f.w > d.x + d.w) fx = d.x + d.w - f.w;
            if (fy + f.h > d.y + d.h) fy = d.y + d.h - f.h;
            s->x += fx - f.x;
            s->y += fy - f.y;
            clamp_toplevel(s);
            f = toplevel_frame(t);
            comp_log("toplevel %d '%s' mapped at %d,%d %dx%d", t->number, t->title, f.x, f.y, f.w, f.h);
            toplevel_activate(t);
        } else if (t->minimized) {
            s->mapped = 1;      /* retains its buffer; hidden by the scene */
        }
        break;
    }
    case ROLE_POPUP: {
        struct popup *p = s->popup;
        s->x = p->parent->x + p->x;
        s->y = p->parent->y + p->y;
        if (first_map)
            comp_log("popup %d shown at %d,%d %dx%d", s->id, s->x, s->y, s->width, s->height);
        break;
    }
    case ROLE_LAYER: {
        struct layer *l = s->layer;
        layer_place(s);
        if (first_map) {
            comp_log("layer surface %d mapped at %d,%d %dx%d", s->id, s->x, s->y, s->width, s->height);
            if (l->layer == LAYER_OVERLAY && l->interactive)
                seat_set_keyboard_focus(s);
            if (l->exclusive > 0) {
                scene_damage_all();
                /* Layers sized to the free area get the new area. */
                for (struct csurface *o = surface_first(); o; o = o->next)
                    if (o != s && o->role == ROLE_LAYER && (o->layer->w <= 0 || o->layer->h <= 0))
                        layer_configure(o->layer);
            }
        }
        break;
    }
    case ROLE_IME_POPUP:
        im_candidates_committed(s, first_map);
        break;
    default:
        break;
    }
}

void shell_output_changed(void)
{
    for (struct csurface *s = surface_first(); s; s = s->next) {
        if (s->role == ROLE_LAYER && s->layer) {
            struct layer *l = s->layer;
            int lr = l->anchor & (LAYER_ANCHOR_LEFT | LAYER_ANCHOR_RIGHT);
            int tb = l->anchor & (LAYER_ANCHOR_TOP | LAYER_ANCHOR_BOTTOM);
            layer_place(s);
            if (l->w <= 0 || l->h <= 0 || lr == (LAYER_ANCHOR_LEFT | LAYER_ANCHOR_RIGHT) ||
                tb == (LAYER_ANCHOR_TOP | LAYER_ANCHOR_BOTTOM))
                layer_configure(l);
        }
    }
    for (struct csurface *s = surface_first(); s; s = s->next) {
        if (s->role != ROLE_TOPLEVEL || !s->toplevel)
            continue;
        struct toplevel *t = s->toplevel;
        if (t->maximized) {
            struct rect d = shell_desktop();
            int top = decor_has(s) ? TITLE_H + BORDER : 0;
            int bw = decor_has(s) ? BORDER : 0;
            s->x = d.x + bw;
            s->y = d.y + top;
            send_configure(t, d.w - 2 * bw, d.h - top - bw);
        } else {
            clamp_toplevel(s);
        }
    }
    scene_damage_all();
}

void shell_surface_gone(struct csurface *s)
{
    if (s->role == ROLE_TOPLEVEL && s->toplevel)
        s->toplevel->s = s;
}

void shell_init(struct wire_server *srv)
{
    server = srv;
    wire_global_create(srv, &shell_interface, 1, bind_shell, NULL);
    wire_global_create(srv, &toplevel_manager_interface, 1, bind_manager, NULL);
}
