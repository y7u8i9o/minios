/* The window policy of the window manager module (wm.h): placement,
 * stacking order, activation, maximized and minimized states, modal
 * windows, the window shortcuts, the decision over a button press and the
 * dimming of the windows of unresponsive clients. */
#include <stdio.h>
#include <string.h>
#include <gui/keymap.h>
#include <minios/input.h>
#include "internal.h"

#define COLOR_DIM 0x00404040

static int next_stack = 1;

void wm_init(void)
{
    decor_init();
}

/* ---- geometry ---- */

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

struct rect wm_extent(const struct csurface *s)
{
    return decor_has(s) ? decor_extent(s) : surface_rect(s);
}

struct rect wm_opaque(const struct csurface *s)
{
    return decor_has(s) ? decor_opaque(s) : surface_rect(s);
}

static void damage_toplevel(struct toplevel *t)
{
    scene_damage(wm_extent(t->s));
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

/* The configure of a maximized toplevel: the desktop area minus the
 * decorations. */
static void maximize_into_desktop(struct toplevel *t)
{
    struct rect d = shell_desktop();
    int top = decor_has(t->s) ? TITLE_H + BORDER : 0;
    int bw = decor_has(t->s) ? BORDER : 0;
    t->s->x = d.x + bw;
    t->s->y = d.y + top;
    shell_configure(t, d.w - 2 * bw, d.h - top - bw);
}

/* ---- activation ---- */

struct toplevel *toplevel_focused(void)
{
    struct toplevel *best = NULL;
    for (struct csurface *s = surface_first(); s; s = s->next)
        if (s->role == ROLE_TOPLEVEL && s->toplevel->activated)
            best = s->toplevel;
    return best;
}

/* The topmost mapped toplevel that is not minimized, or NULL. */
static struct toplevel *topmost_visible(void)
{
    struct toplevel *best = NULL;
    for (struct csurface *s = surface_first(); s; s = s->next)
        if (s->role == ROLE_TOPLEVEL && !s->toplevel->minimized && s->mapped && (!best || s->stack > best->s->stack))
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

/* A toplevel with a modal child is blocked. The modal child is activated
 * instead, and the function returns 1. */
static int toplevel_blocked(struct toplevel *t)
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
        damage_toplevel(old);
        toplevel_configure_size(old, &w, &h);
        shell_configure(old, w, h);
        shell_toplevel_state_changed(old);
    }
    t->activated = 1;
    t->s->stack = next_stack++;
    damage_toplevel(t);
    toplevel_configure_size(t, &w, &h);
    shell_configure(t, w, h);
    shell_toplevel_state_changed(t);
    seat_set_keyboard_focus(t->s);
    comp_log("toplevel %d activated", t->number);
}

void toplevel_set_maximized(struct toplevel *t, int on)
{
    if (t->maximized == !!on)
        return;
    damage_toplevel(t);
    if (on) {
        t->saved_x = t->s->x;
        t->saved_y = t->s->y;
        toplevel_configure_size(t, &t->saved_w, &t->saved_h);
        t->maximized = 1;
        maximize_into_desktop(t);
        comp_log("toplevel %d maximized", t->number);
    } else {
        t->maximized = 0;
        t->s->x = t->saved_x;
        t->s->y = t->saved_y;
        shell_configure(t, t->saved_w, t->saved_h);
        comp_log("toplevel %d restored", t->number);
    }
    damage_toplevel(t);
    shell_toplevel_state_changed(t);
}

void toplevel_set_minimized(struct toplevel *t, int on)
{
    if (t->minimized == !!on)
        return;
    t->minimized = !!on;
    damage_toplevel(t);
    comp_log("toplevel %d %s", t->number, on ? "minimized" : "shown");
    if (on && t->activated) {
        t->activated = 0;
        int w, h;
        toplevel_configure_size(t, &w, &h);
        shell_configure(t, w, h);
        struct toplevel *next = topmost_visible();
        seat_set_keyboard_focus(NULL);
        if (next)
            toplevel_activate(next);
    } else if (!on) {
        toplevel_activate(t);
    }
    shell_toplevel_state_changed(t);
}

void toplevel_close(struct toplevel *t)
{
    struct toplevel *modal = modal_child(t);
    shell_request_close(modal ? modal : t);
}

void toplevel_move(struct toplevel *t, int x, int y)
{
    damage_toplevel(t);
    t->s->x = x;
    t->s->y = y;
    clamp_toplevel(t->s);
    damage_toplevel(t);
}

/* Alt+Tab activates the lowest visible toplevel. Repeated presses cycle
 * through all of them. */
static void toplevel_cycle(void)
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

/* ---- events of the core ---- */

void wm_toplevel_created(struct toplevel *t)
{
    t->s->stack = next_stack++;
}

void wm_toplevel_committed(struct toplevel *t, int first_map)
{
    struct csurface *s = t->s;
    if (first_map && !t->placed) {
        /* Placement works on the visible frame: centred on the parent,
         * or cascading from (40, 30) of the desktop area. The cascade
         * puts the contents under a 30 pixel toolkit header bar at y 60. */
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
        s->mapped = 1;      /* the surface retains its buffer, and the scene hides the surface */
    }
}

void wm_toplevel_destroyed(struct toplevel *t, int was_active)
{
    decor_toplevel_gone(t);
    decor_free(t);
    for (struct csurface *s = surface_first(); s; s = s->next)
        if (s->role == ROLE_TOPLEVEL && s->toplevel && s->toplevel->parent == t)
            s->toplevel->parent = NULL;
    if (was_active) {
        struct toplevel *next = topmost_visible();
        if (next)
            toplevel_activate(next);
    }
}

void wm_output_changed(void)
{
    for (struct csurface *s = surface_first(); s; s = s->next) {
        if (s->role != ROLE_TOPLEVEL || !s->toplevel)
            continue;
        if (s->toplevel->maximized)
            maximize_into_desktop(s->toplevel);
        else
            clamp_toplevel(s);
    }
}

/* The button must still be pressed. A request that arrives after the
 * release (a client that answered late) would drag with no button. */
void wm_request_move(struct toplevel *t)
{
    if (!t->maximized && seat_buttons())
        decor_press(t->s, 1 | 0x100);
}

void wm_request_resize(struct toplevel *t, int edges)
{
    if (!t->maximized && seat_buttons())
        decor_press(t->s, 1 | 0x200 | (edges << 16));
}

/* ---- input ---- */

/* covers_decorations is 1 when target, the surface under the cursor, lies
 * above the decorations of the toplevel s. A toplevel covers them when it
 * is higher in the stack. Layer surfaces of the top and overlay layers,
 * popups and the other roles lie above every toplevel. */
static int covers_decorations(const struct csurface *target, const struct csurface *s)
{
    if (!target)
        return 0;
    if (target->role == ROLE_TOPLEVEL)
        return target->stack > s->stack;
    if (target->role == ROLE_LAYER)
        return target->layer->layer >= 2;
    return 1;
}

int wm_pointer_press(struct csurface *target, int button)
{
    /* The decorations of the toplevel under the cursor. */
    for (struct csurface *s = surface_first(); s; s = s->next)
        if (decor_hit(s, cursor_x, cursor_y) && !s->toplevel->minimized && !covers_decorations(target, s)) {
            if (toplevel_blocked(s->toplevel))
                return WM_BLOCKED;
            toplevel_activate(s->toplevel);
            decor_press(s, button);
            return WM_USED;
        }
    if (!target || target->role != ROLE_TOPLEVEL)
        return WM_PASS;
    /* The contents of a dimmed window take no clicks. */
    if (target->client->unresponsive)
        return WM_USED;
    if (toplevel_blocked(target->toplevel))
        return WM_BLOCKED;
    toplevel_activate(target->toplevel);
    if (button == 1 && (seat_modifiers() & KEYMAP_MOD_ALT)) {
        decor_press(target, 1);
        return WM_USED;
    }
    return WM_PASS;
}

int wm_pointer_motion(void)
{
    return decor_motion();
}

int wm_pointer_release(void)
{
    return decor_release();
}

const char *wm_cursor(struct csurface *target)
{
    const char *drag = decor_cursor(NULL, cursor_x, cursor_y);
    if (drag)
        return drag;
    for (struct csurface *s = surface_first(); s; s = s->next)
        if (decor_hit(s, cursor_x, cursor_y) && !s->toplevel->minimized && !covers_decorations(target, s))
            return decor_cursor(s, cursor_x, cursor_y);
    /* The contents of a dimmed window show the wait cursor. */
    if (target && target->role == ROLE_TOPLEVEL && target->client->unresponsive)
        return "wait";
    return NULL;
}

int wm_key(uint32_t key, int mods)
{
    /* An overlay with the keyboard, such as the login window or the
     * authentication dialog, retains the keyboard: Alt+Tab and Alt+F4 go
     * to the overlay instead of the windows below the overlay. */
    struct csurface *focus = seat_keyboard_focus();
    if (focus && focus->role == ROLE_LAYER && focus->layer->layer == LAYER_OVERLAY)
        return 0;
    if (!(mods & KEYMAP_MOD_ALT))
        return 0;
    if (key == KEY_TAB) {
        toplevel_cycle();
        return 1;
    }
    if (key == KEY_F4) {
        struct toplevel *t = toplevel_focused();
        if (t)
            toplevel_close(t);
        return 1;
    }
    return 0;
}

/* ---- drawing ---- */

void wm_draw_below(struct csurface *s, struct rect clip)
{
    if (!decor_has(s))
        return;
    decor_draw_shadow(s, clip);
    decor_draw(s, clip);
}

/* The contents of every toplevel of an unresponsive client are dimmed. */
void wm_draw_above(struct csurface *s, struct rect clip)
{
    if (s->role != ROLE_TOPLEVEL || !s->toplevel || !s->client->unresponsive)
        return;
    struct rect c = rect_intersect(surface_rect(s), clip);
    if (rect_empty(c))
        return;
    int S = screen_scale;
    struct rect d = rect_intersect((struct rect){ c.x * S, c.y * S, c.w * S, c.h * S },
                                   (struct rect){ 0, 0, back.width, back.height });
    for (int y = d.y; y < d.y + d.h; y++) {
        uint32_t *row = &back.pixels[(size_t)y * back.stride];
        for (int x = d.x; x < d.x + d.w; x++)
            row[x] = ((row[x] >> 1) & 0x007f7f7f) + (COLOR_DIM >> 1);
    }
}
