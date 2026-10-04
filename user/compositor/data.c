/* Data device: the selection and drag and drop between clients. Offers
 * are created per receiving client; contents flow through the pipe
 * descriptor the receiver passes to receive, forwarded to the source
 * with send. A drag carries an action, copy or move, chosen from what
 * the source and the target allow, the target's preference and the
 * modifiers. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <gui/keymap.h>
#include "comp.h"

#define MAX_MIME 8
#define ACTION_COPY 1
#define ACTION_MOVE 2
#define ACTIONS (ACTION_COPY | ACTION_MOVE)

struct source {
    struct wire_resource *res;
    struct client *client;
    char mimes[MAX_MIME][48];
    int nmimes;
    int is_drag;
    uint32_t actions;           /* allowed actions; copy when never set */
    uint32_t action;            /* the action last sent to the source */
};

struct offer {
    struct wire_resource *res;
    struct source *source;      /* NULL once the source is gone and nothing replaced it */
    int is_drag;
    uint32_t enter_serial;
    char accepted[48];
    int dropped;
    uint32_t actions, preferred;    /* of the target; copy and move with copy preferred when never set */
    uint32_t action;            /* the action last sent to the target */
    struct offer *next;         /* in the list of live offers */
};

/* Every offer not yet destroyed, so that a source that goes away can
 * detach the offers made for it. */
static struct offer *offers;

static struct source *selection;
static struct source *drag_source;
/* Stored copy of the selection: fetched when it is set, served by the
 * compositor itself once the owner is gone. */
static char store_mime[48];
static char *store_data;
static size_t store_len;
static int fetch_fd = -1;
static char *fetch_buf;
static size_t fetch_len;
static struct source stored_source;
static struct csurface *drag_icon, *drag_target;
static struct offer *drag_offer;

int data_dragging(void) { return drag_source != NULL; }

static void drag_finish(int drop);
static void drag_update_action(void);

static const char *action_name(uint32_t a) { return a == ACTION_MOVE ? "move" : a == ACTION_COPY ? "copy" : "none"; }

/* ---- sources ---- */

static void h_source_offer(struct wire_client *c, struct wire_resource *self, const char *mime)
{
    struct source *s = self->data;
    if (s->nmimes < MAX_MIME)
        strlcpy(s->mimes[s->nmimes++], mime, sizeof s->mimes[0]);
}
static void h_source_destroy(struct wire_client *c, struct wire_resource *self) { wire_resource_destroy(self); }
static void h_source_set_actions(struct wire_client *c, struct wire_resource *self, uint32_t actions)
{
    struct source *s = self->data;
    s->actions = actions & ACTIONS;
    if (s == drag_source)
        drag_update_action();
}
static const struct data_source_impl source_handlers = { h_source_offer, h_source_destroy, h_source_set_actions };

/* Start reading the selection into the store. */
static void fetch_start(struct source *s)
{
    if (fetch_fd >= 0) {
        close(fetch_fd);
        fetch_fd = -1;
    }
    free(fetch_buf);
    fetch_buf = NULL;
    fetch_len = 0;
    if (!s->nmimes || !s->res)
        return;
    int p[2];
    if (pipe2(p, O_CLOEXEC | O_NONBLOCK) < 0)
        return;
    strlcpy(store_mime, s->mimes[0], sizeof store_mime);
    data_source_send_send(s->res, s->mimes[0], p[1]);
    close(p[1]);
    fetch_fd = p[0];
}

int data_fetch_fd(void) { return fetch_fd; }

void data_fetch_read(void)
{
    char buf[4096];
    ssize_t n = read(fetch_fd, buf, sizeof buf);
    if (n > 0 && fetch_len + (size_t)n <= 65536) {
        fetch_buf = realloc(fetch_buf, fetch_len + (size_t)n);
        memcpy(fetch_buf + fetch_len, buf, (size_t)n);
        fetch_len += (size_t)n;
        return;
    }
    if (n < 0 && errno == EAGAIN)
        return;
    close(fetch_fd);
    fetch_fd = -1;
    free(store_data);
    store_data = fetch_buf;
    store_len = fetch_len;
    fetch_buf = NULL;
    fetch_len = 0;
    comp_log("selection stored, %d bytes", (int)store_len);
}

static void use_store(void)
{
    if (!store_data || !store_len)
        return;
    memset(&stored_source, 0, sizeof stored_source);
    strlcpy(stored_source.mimes[0], store_mime, sizeof stored_source.mimes[0]);
    stored_source.nmimes = 1;
    selection = &stored_source;
}

static void source_gone(struct wire_resource *r)
{
    struct source *s = r->data;
    int was_selection = selection == s;
    if (was_selection) {
        selection = NULL;
        use_store();
    }
    /* Another client may have an offer of this source, because the focus
     * moves to it when the owner's window closes, before the owner
     * disconnects. Such an offer now reads the stored copy, or nothing. */
    for (struct offer *o = offers; o; o = o->next)
        if (o->source == s)
            o->source = was_selection && selection == &stored_source ? &stored_source : NULL;
    /* A drag whose source goes away is cancelled; the resource can no
     * longer receive events. */
    if (drag_source == s) {
        s->res = NULL;
        drag_finish(0);
    }
    free(s);
}

static void h_create_source(struct wire_client *c, struct wire_resource *self, uint32_t id)
{
    struct source *s = calloc(1, sizeof *s);
    struct wire_resource *r = s ? wire_resource_create(c, &data_source_interface, self->obj.version, id) : NULL;
    if (!r) {
        free(s);
        return;
    }
    s->res = r;
    s->client = wire_client_get_user_data(c);
    s->actions = ACTION_COPY;
    wire_resource_set_listener(r, &source_handlers, s, source_gone);
}

/* ---- offers ---- */

static int source_has_mime(const struct source *s, const char *mime)
{
    if (!s || !mime)
        return 0;
    for (int i = 0; i < s->nmimes; i++)
        if (strcmp(s->mimes[i], mime) == 0)
            return 1;
    return 0;
}

static void h_offer_accept(struct wire_client *c, struct wire_resource *self, uint32_t serial, const char *mime)
{
    struct offer *o = self->data;
    if (!o->is_drag || serial != o->enter_serial)
        return;
    if (mime && !source_has_mime(o->source, mime))
        return;
    strlcpy(o->accepted, mime ? mime : "", sizeof o->accepted);
}
static void h_offer_receive(struct wire_client *c, struct wire_resource *self, const char *mime, int fd)
{
    struct offer *o = self->data;
    if (!source_has_mime(o->source, mime)) {
        close(fd);
        return;
    }
    if (o->source == &stored_source) {
        size_t off = 0;
        while (store_data && off < store_len) {
            ssize_t n = write(fd, store_data + off, store_len - off);
            if (n <= 0)
                break;
            off += (size_t)n;
        }
    } else if (o->source && o->source->res) {
        data_source_send_send(o->source->res, mime, fd);
    }
    close(fd);
    comp_log("offer received as %s", mime);
}
static void h_offer_finish(struct wire_client *c, struct wire_resource *self)
{
    struct offer *o = self->data;
    if (o->is_drag && o->dropped && o->accepted[0] && o->source && o->source->res)
        data_source_send_dnd_finished(o->source->res);
}
static void h_offer_destroy(struct wire_client *c, struct wire_resource *self) { wire_resource_destroy(self); }
static void h_offer_set_actions(struct wire_client *c, struct wire_resource *self, uint32_t actions, uint32_t preferred)
{
    struct offer *o = self->data;
    if (!o->is_drag)
        return;
    o->actions = actions & ACTIONS;
    o->preferred = preferred & o->actions;
    if (o == drag_offer)
        drag_update_action();
}
static const struct data_offer_impl offer_handlers = { h_offer_accept, h_offer_receive, h_offer_finish, h_offer_destroy,
                                                       h_offer_set_actions };
static void offer_gone(struct wire_resource *r)
{
    struct offer *o = r->data;
    if (drag_offer == o)
        drag_offer = NULL;
    for (struct offer **p = &offers; *p; p = &(*p)->next)
        if (*p == o) {
            *p = o->next;
            break;
        }
    free(o);
}

/* A new offer for a source announced to a client's data device. */
static struct offer *offer_create(struct client *cl, struct source *src, int is_drag)
{
    if (!cl->data_device)
        return NULL;
    struct offer *o = calloc(1, sizeof *o);
    int version = cl->data_device->obj.version;
    struct wire_resource *r = o ? wire_resource_create(cl->wc, &data_offer_interface, version, 0) : NULL;
    if (!r) {
        free(o);
        return NULL;
    }
    o->res = r;
    o->source = src;
    o->is_drag = is_drag;
    o->actions = ACTIONS;
    o->preferred = ACTION_COPY;
    o->next = offers;
    offers = o;
    wire_resource_set_listener(r, &offer_handlers, o, offer_gone);
    data_device_send_data_offer(cl->data_device, r);
    for (int i = 0; i < src->nmimes; i++)
        data_offer_send_offer(r, src->mimes[i]);
    if (is_drag && version >= 2)
        data_offer_send_source_actions(r, src->actions);
    return o;
}

/* ---- devices ---- */

static void h_set_selection(struct wire_client *c, struct wire_resource *self, struct wire_resource *source, uint32_t serial)
{
    struct source *s = source ? source->data : NULL;
    struct client *cl = wire_client_get_user_data(c);
    if (!cl || !seat_validate_serial(cl, serial) || (s && s->client != cl))
        return;
    if (selection && selection != s && selection->res)
        data_source_send_cancelled(selection->res);
    selection = s;
    comp_log("selection set by client %d (%s)", cl ? cl->number : 0, s && s->nmimes ? s->mimes[0] : "none");
    if (s)
        fetch_start(s);
    struct csurface *focus = seat_keyboard_focus();
    if (focus)
        data_keyboard_focus_changed(focus->client);
}

static void h_start_drag(struct wire_client *c, struct wire_resource *self, struct wire_resource *source,
                         struct wire_resource *origin, struct wire_resource *icon, uint32_t serial)
{
    struct client *cl = wire_client_get_user_data(c);
    struct source *src = source ? source->data : NULL;
    struct csurface *from = origin ? origin->data : NULL;
    struct csurface *ic = icon ? icon->data : NULL;
    if (!cl || !src || !from || src->client != cl || from->client != cl ||
        (ic && (ic->client != cl || ic->role != ROLE_NONE)) || !seat_validate_drag(cl, from, serial))
        return;
    drag_source = src;
    drag_source->is_drag = 1;
    drag_source->action = 0;
    drag_icon = ic;
    if (drag_icon) {
        /* The icon's attach offsets move it relative to the cursor. */
        drag_icon->role = ROLE_DND_ICON;
        drag_icon->hotspot_x = drag_icon->hotspot_y = 0;
        drag_icon->x = cursor_x;
        drag_icon->y = cursor_y;
    }
    drag_target = NULL;
    drag_offer = NULL;
    seat_drag_started();
    comp_log("drag started by client %d", drag_source->client->number);
    scene_damage_all();
    data_pointer_motion();
}
static void h_device_destroy(struct wire_client *c, struct wire_resource *self) { wire_resource_destroy(self); }
static const struct data_device_impl device_handlers = { h_set_selection, h_start_drag, h_device_destroy };
static void device_gone(struct wire_resource *r)
{
    struct client *cl = wire_client_get_user_data(r->client);
    if (cl && cl->data_device == r)
        cl->data_device = NULL;
}

static void h_get_device(struct wire_client *c, struct wire_resource *self, uint32_t id, struct wire_resource *seat)
{
    struct client *cl = wire_client_get_user_data(c);
    struct wire_resource *r = wire_resource_create(c, &data_device_interface, self->obj.version, id);
    if (!r || !cl)
        return;
    cl->data_device = r;
    wire_resource_set_listener(r, &device_handlers, NULL, device_gone);
    if (selection)
        data_keyboard_focus_changed(cl);        /* single user: every client sees the selection */
}
static const struct data_device_manager_impl manager_handlers = { h_create_source, h_get_device };

static void bind_manager(struct wire_client *c, void *data, uint32_t version, uint32_t id)
{
    struct wire_resource *r = wire_resource_create(c, &data_device_manager_interface, (int)version, id);
    if (r)
        wire_resource_set_listener(r, &manager_handlers, NULL, NULL);
}

void data_init(struct wire_server *srv)
{
    wire_global_create(srv, &data_device_manager_interface, 2, bind_manager, NULL);
}

/* The client with keyboard focus learns about the current selection. */
void data_keyboard_focus_changed(struct client *cl)
{
    if (!cl->data_device)
        return;
    if (!selection) {
        data_device_send_selection(cl->data_device, NULL);
        return;
    }
    struct offer *o = offer_create(cl, selection, 0);
    data_device_send_selection(cl->data_device, o ? o->res : NULL);
}

/* ---- drag and drop ---- */

/* The action of the drag over the current target: Ctrl forces copy and
 * Shift forces move when both sides allow it, otherwise the target's
 * preference, else copy, else move. 0 when no action is allowed. */
static uint32_t drag_choose_action(void)
{
    if (!drag_source || !drag_offer)
        return 0;
    uint32_t both = drag_source->actions & drag_offer->actions;
    int mods = seat_modifiers();
    if ((mods & KEYMAP_MOD_CTRL) && (both & ACTION_COPY))
        return ACTION_COPY;
    if ((mods & KEYMAP_MOD_SHIFT) && (both & ACTION_MOVE))
        return ACTION_MOVE;
    if (both & drag_offer->preferred & ACTION_COPY)
        return ACTION_COPY;
    if (both & drag_offer->preferred & ACTION_MOVE)
        return ACTION_MOVE;
    return both & ACTION_COPY ? ACTION_COPY : both & ACTION_MOVE;
}

/* Tell the target and the source when the chosen action changes. */
static void drag_update_action(void)
{
    uint32_t a = drag_choose_action();
    if (drag_offer && a != drag_offer->action) {
        drag_offer->action = a;
        if (drag_offer->res->obj.version >= 2)
            data_offer_send_action(drag_offer->res, a);
    }
    if (drag_source && a != drag_source->action) {
        drag_source->action = a;
        if (drag_source->res && drag_source->res->obj.version >= 2)
            data_source_send_action(drag_source->res, a);
        comp_log("drag action %s", action_name(a));
    }
}

void data_drag_modifiers(void) { drag_update_action(); }

void data_icon_committed(struct csurface *s, int attach_x, int attach_y)
{
    s->hotspot_x -= attach_x;
    s->hotspot_y -= attach_y;
    s->x = cursor_x - s->hotspot_x;
    s->y = cursor_y - s->hotspot_y;
}

void data_pointer_motion(void)
{
    if (drag_icon && drag_icon->mapped) {
        scene_damage(surface_rect(drag_icon));
        drag_icon->x = cursor_x - drag_icon->hotspot_x;
        drag_icon->y = cursor_y - drag_icon->hotspot_y;
        scene_damage(surface_rect(drag_icon));
    }
    struct csurface *t = scene_surface_at(cursor_x, cursor_y);
    if (t == drag_icon)
        t = NULL;
    if (t != drag_target) {
        if (drag_target && drag_target->client->data_device)
            data_device_send_leave(drag_target->client->data_device);
        drag_target = t;
        drag_offer = NULL;
        if (t && t->client->data_device) {
            drag_offer = offer_create(t->client, drag_source, 1);
            uint32_t enter_serial = comp_serial();
            if (drag_offer)
                drag_offer->enter_serial = enter_serial;
            data_device_send_enter(t->client->data_device, enter_serial, t->res, (int32_t)((cursor_fx - t->x) * 256.0),
                                   (int32_t)((cursor_fy - t->y) * 256.0), drag_offer ? drag_offer->res : NULL);
            comp_log("drag enter surface %d", t->id);
        }
        drag_update_action();
    } else if (t && t->client->data_device) {
        data_device_send_motion(t->client->data_device, (uint32_t)uptime_ms(), (int32_t)((cursor_fx - t->x) * 256.0),
                                (int32_t)((cursor_fy - t->y) * 256.0));
    }
}

/* End the drag. With drop set, a target that accepted a type and has an
 * action receives the drop; otherwise the target receives leave and the
 * source cancelled. The icon is unmapped and the pointer returns to the
 * surface under it. */
static void drag_finish(int drop)
{
    struct source *src = drag_source;
    if (!src)
        return;
    struct wire_resource *dev = drag_target ? drag_target->client->data_device : NULL;
    if (drop && dev && drag_offer && drag_offer->accepted[0] && drag_offer->action) {
        drag_offer->dropped = 1;
        data_device_send_drop(dev);
        if (src->res)
            data_source_send_dnd_drop_performed(src->res);
        comp_log("drop on surface %d, action %s", drag_target->id, action_name(drag_offer->action));
    } else {
        if (dev)
            data_device_send_leave(dev);
        if (src->res)
            data_source_send_cancelled(src->res);
        comp_log("drag cancelled");
    }
    drag_source = NULL;
    if (drag_icon) {
        scene_damage(surface_rect(drag_icon));
        drag_icon->role = ROLE_NONE;
        drag_icon->mapped = 0;
        drag_icon = NULL;
    }
    drag_target = NULL;
    drag_offer = NULL;
    seat_drag_ended(seat_buttons() != 0);
}

void data_pointer_release(void) { drag_finish(1); }
void data_drag_cancel(void) { drag_finish(0); }

void data_surface_gone(struct csurface *s)
{
    if (drag_icon == s)
        drag_icon = NULL;
    if (drag_target == s)
        drag_target = NULL;
}

void data_client_gone(struct client *c)
{
    if (selection && selection->client == c) {
        selection = NULL;
        use_store();
    }
    if (drag_source && drag_source->client == c)
        drag_finish(0);
}
