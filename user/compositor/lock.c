/* The session lock (S6 of docs/plan/release-0.7.0.md, docs/design/lock.md).
 *
 * A screen locker binds session_lock_manager and locks the session. While
 * the session is locked, the scene shows only the lock surface of the
 * locker and its popups, and the seat sends all input to them. The
 * locked event follows the first composed frame of the locked session.
 * Only the locker that locked the session can unlock it.
 *
 * A locker that exits without unlocking leaves the session locked. X12
 * then shows a black screen with a message. A key press asks the session
 * manager for a new locker (session.c). While no locker runs, X12 accepts
 * a lock request only from root or from the locker that the session
 * manager started and reported with set_locker.
 *
 * The session manager sessiond starts /bin/lock for Super+L and after
 * lock_timeout seconds without input (docs/design/sessiond.md). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <gui/paint.h>
#include "comp.h"

/* The object of one lock request. */
struct lock_request {
    struct wire_resource *res;
    struct lock_surface_state *surface;
};

static int locked;                      /* the session is locked */
static struct lock_request *owner;      /* the request that locked the session, NULL when no locker owns it */
static int locked_owed;                 /* owner waits for the locked event */
static int locker_pid;                  /* the locker that the session manager started, 0 for none */

int lock_active(void)
{
    return locked;
}

struct csurface *lock_surface_mapped(void)
{
    if (!locked || !owner || !owner->surface || !owner->surface->s)
        return NULL;
    struct csurface *s = owner->surface->s;
    return s->mapped && s->current.buffer ? s : NULL;
}

/* Give the keyboard focus to the lock surface while the session is
 * locked. A locked session without a mapped lock surface has no keyboard
 * focus. An unlocked session returns the focus to the active window. Then
 * update the pointer focus. */
static void refocus(void)
{
    struct csurface *view = lock_surface_mapped();
    if (view) {
        seat_set_keyboard_focus(view);
    } else if (locked) {
        seat_set_keyboard_focus(NULL);
    } else {
        struct toplevel *t = toplevel_focused();
        seat_set_keyboard_focus(t && t->s ? t->s : NULL);
    }
    seat_pointer_motion();
}

static void begin_lock(void)
{
    locked = 1;
    popup_dismiss_all();
    if (data_dragging())
        data_drag_cancel();
    refocus();
    scene_damage_all();
}

static void end_lock(void)
{
    locked = 0;
    locked_owed = 0;
    scene_damage_all();
    refocus();
    comp_log("session unlocked");
}

/* ---- lock surfaces ---- */

static void send_configure(struct lock_surface_state *l)
{
    l->serial = comp_serial();
    l->pending_w = screen_w;
    l->pending_h = screen_h;
    lock_surface_send_configure(l->res, l->serial, screen_w, screen_h);
}

static void h_surface_ack(struct wire_client *c, struct wire_resource *self, uint32_t serial)
{
    struct lock_surface_state *l = self->data;
    if (!l->serial || serial != l->serial) {
        wire_client_post_error(c, self, 30, "invalid lock surface configure serial");
        return;
    }
    l->acked_serial = serial;
}

static void h_surface_destroy(struct wire_client *c, struct wire_resource *self) { wire_resource_destroy(self); }
static const struct lock_surface_impl lock_surface_handlers = { h_surface_ack, h_surface_destroy };

static void lock_surface_resource_gone(struct wire_resource *r)
{
    struct lock_surface_state *l = r->data;
    struct csurface *s = l->s;
    if (s) {
        int shown = s == lock_surface_mapped();
        s->role = ROLE_NONE;
        s->lock = NULL;
        s->mapped = 0;
        seat_surface_gone(s);
        if (shown) {
            scene_damage_all();
            refocus();
        }
    }
    if (owner && owner->surface == l)
        owner->surface = NULL;
    free(l);
}

void lock_surface_committed(struct csurface *s, int first_map)
{
    s->x = 0;
    s->y = 0;
    if (!first_map)
        return;
    comp_log("lock surface %d mapped %dx%d", s->id, s->width, s->height);
    if (s == lock_surface_mapped()) {
        scene_damage_all();
        refocus();
    }
}

void lock_surface_gone(struct csurface *s)
{
    if (s->lock)
        s->lock->s = NULL;
}

void lock_output_changed(void)
{
    for (struct csurface *s = surface_first(); s; s = s->next)
        if (s->role == ROLE_LOCK && s->lock && s->lock->res)
            send_configure(s->lock);
}

/* ---- lock requests ---- */

static void h_get_lock_surface(struct wire_client *c, struct wire_resource *self, uint32_t id,
                               struct wire_resource *surface)
{
    struct lock_request *q = self->data;
    struct csurface *s = surface->data;
    if (s->role != ROLE_NONE) {
        wire_client_post_error(c, self, 20, "surface already has a role");
        return;
    }
    if (q->surface) {
        wire_client_post_error(c, self, 31, "the lock has a lock surface");
        return;
    }
    struct lock_surface_state *l = calloc(1, sizeof *l);
    struct wire_resource *r = l ? wire_resource_create(c, &lock_surface_interface, 1, id) : NULL;
    if (!r) {
        free(l);
        return;
    }
    l->s = s;
    l->res = r;
    s->role = ROLE_LOCK;
    s->lock = l;
    q->surface = l;
    wire_resource_set_listener(r, &lock_surface_handlers, l, lock_surface_resource_gone);
    send_configure(l);
}

static void h_unlock(struct wire_client *c, struct wire_resource *self)
{
    struct lock_request *q = self->data;
    if (q != owner || locked_owed) {
        wire_client_post_error(c, self, 32, "unlock without a lock");
        return;
    }
    owner = NULL;
    end_lock();
    wire_resource_destroy(self);
}

static void h_lock_destroy(struct wire_client *c, struct wire_resource *self) { wire_resource_destroy(self); }
static const struct session_lock_impl session_lock_handlers = { h_get_lock_surface, h_unlock, h_lock_destroy };

/* The lock object ends by unlock_and_destroy, by destroy or with the
 * connection of its client. After destroy or the end of the connection,
 * the session remains locked without a locker. */
static void session_lock_gone(struct wire_resource *r)
{
    struct lock_request *q = r->data;
    if (q == owner) {
        owner = NULL;
        locked_owed = 0;
        comp_log("the locker exited without unlocking the session");
        scene_damage_all();
        refocus();
    }
    free(q);
}

static void h_lock(struct wire_client *c, struct wire_resource *self, uint32_t id)
{
    struct client *cl = wire_client_get_user_data(c);
    struct lock_request *q = calloc(1, sizeof *q);
    struct wire_resource *r = q ? wire_resource_create(c, &session_lock_interface, 1, id) : NULL;
    if (!r || !cl) {
        free(q);
        return;
    }
    q->res = r;
    wire_resource_set_listener(r, &session_lock_handlers, q, session_lock_gone);
    const char *refusal = NULL;
    if (locked && owner)
        refusal = "the session is locked";
    else if (locked && cl->uid != 0 && (locker_pid <= 0 || cl->peer_pid != locker_pid))
        refusal = "a locker started by the session manager is expected";
    if (refusal) {
        session_lock_send_finished(r);
        comp_log("lock of client %d refused: %s", cl->number, refusal);
        return;
    }
    int relock = locked;
    owner = q;
    locked_owed = 1;
    if (relock)
        scene_damage_all();
    else
        begin_lock();
    comp_log(relock ? "client %d resumes the lock" : "client %d locks the session", cl->number);
}

static const struct session_lock_manager_impl manager_handlers = { h_lock };

static void bind_manager(struct wire_client *c, void *data, uint32_t version, uint32_t id)
{
    struct wire_resource *r = wire_resource_create(c, &session_lock_manager_interface, (int)version, id);
    if (r)
        wire_resource_set_listener(r, &manager_handlers, NULL, NULL);
}

void lock_init(struct wire_server *srv)
{
    wire_global_create(srv, &session_lock_manager_interface, 1, bind_manager, NULL);
}

void lock_frame_done(void)
{
    if (!locked_owed || !owner)
        return;
    locked_owed = 0;
    session_lock_send_locked(owner->res);
    comp_log("session locked");
}

void lock_session_ended(void)
{
    if (!locked)
        return;
    if (owner) {
        session_lock_send_finished(owner->res);
        owner = NULL;
    }
    end_lock();
}

/* ---- the locker ---- */

void lock_set_locker(int pid)
{
    locker_pid = pid;
    comp_log("locker %d reported", pid);
}

void lock_key_without_locker(void)
{
    if (locked && !owner)
        session_locker_needed();
}

/* ---- drawing ---- */

void lock_draw(struct rect clip)
{
    if (!locked || owner)
        return;
    static const char *const lines[] = {
        "The screen locker has stopped.",
        "Press a key to start it again.",
    };
    const struct font *f = scene_font();
    int S = screen_scale, line_h = f->height + 8;
    int top = (screen_h - 2 * line_h) / 2;
    struct painter p;
    painter_init_scaled(&p, &back, scene_theme(), S);
    painter_push(&p, clip.x, clip.y, clip.w, clip.h);
    for (int i = 0; i < 2; i++) {
        int tw = (gfx_text_width_font_scaled(f, lines[i], -1, S) + S - 1) / S;
        painter_text_font(&p, f, (screen_w - tw) / 2 - clip.x, top + i * line_h - clip.y, lines[i], 0x00d0d0d0,
                          0xffffffffu);
    }
    painter_pop(&p);
}
