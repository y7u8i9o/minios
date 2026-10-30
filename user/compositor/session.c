/* The session manager interface (protocol/session.xml,
 * docs/design/sessiond.md).
 *
 * X12 reports the events of the session to the session manager sessiond:
 * the presses of the shortcut keys that sessiond bound, the idle time, a
 * key press in a locked session without a locker, and the clients that
 * stop or resume answering pings. sessiond starts the programs and ends
 * the processes. X12 itself starts no program.
 *
 * Only a client of root or of the user that runs X12 may bind the global.
 * The state below belongs to the single thread of X12. */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <gui/keymap.h>
#include "comp.h"

#define MAX_SHORTCUTS 16

/* A key bound by a session manager. */
struct shortcut {
    struct wire_resource *owner;
    uint32_t key, mods;
    char name[32];
};

static struct wire_server *server;
static struct shortcut shortcuts[MAX_SHORTCUTS];
static int nshortcuts;
static long last_input;                 /* uptime ms of the last input */
static int idle_sent;                   /* the idle event of this period without input went out */

/* Send an event to every bound session manager. */
static void for_each_manager(void (*send)(struct wire_resource *r, const void *arg), const void *arg)
{
    for (struct wire_client *k = wire_server_first_client(server); k; k = wire_client_next(k))
        for (struct wire_resource *r = wire_client_first_resource(k); r; r = r->next)
            if (r->obj.interface == &session_manager_interface)
                send(r, arg);
}

static void send_user(struct wire_resource *r, const void *arg)
{
    (void)arg;
    session_manager_send_user(r, session_uid);
}

/* ---- requests ---- */

static void h_set_user(struct wire_client *c, struct wire_resource *self, int32_t uid)
{
    if (uid < -1) {
        wire_client_post_error(c, self, 40, "invalid session user");
        return;
    }
    session_uid = uid;
    comp_log("session uid %d", uid);
    if (uid < 0)
        lock_session_ended();
    clients_drop_disallowed();
    idle_sent = 0;
    last_input = uptime_ms();
    for_each_manager(send_user, NULL);
}

static void h_bind_shortcut(struct wire_client *c, struct wire_resource *self, uint32_t key, uint32_t mods,
                            const char *name)
{
    struct shortcut *s = NULL;
    for (int i = 0; i < nshortcuts && !s; i++)
        if (shortcuts[i].key == key && shortcuts[i].mods == mods)
            s = &shortcuts[i];
    if (!s && nshortcuts == MAX_SHORTCUTS) {
        comp_log("shortcut %s refused: the table is full", name);
        return;
    }
    if (!s)
        s = &shortcuts[nshortcuts++];
    s->owner = self;
    s->key = key;
    s->mods = mods;
    strlcpy(s->name, name, sizeof s->name);
    comp_log("shortcut %s bound", s->name);
}

static void h_set_locker(struct wire_client *c, struct wire_resource *self, uint32_t pid)
{
    lock_set_locker((int)pid);
}

static void h_drop_client(struct wire_client *c, struct wire_resource *self, uint32_t number)
{
    for (struct wire_client *k = wire_server_first_client(server); k; k = wire_client_next(k)) {
        struct client *cl = wire_client_get_user_data(k);
        if (cl && k != c && (uint32_t)cl->number == number) {
            comp_log("client %d dropped by the session manager", cl->number);
            wire_client_destroy(k);
            return;
        }
    }
}

static void h_destroy(struct wire_client *c, struct wire_resource *self) { wire_resource_destroy(self); }

static const struct session_manager_impl manager_handlers = { h_set_user, h_bind_shortcut, h_set_locker,
                                                              h_drop_client, h_destroy };

/* The shortcuts of a session manager end with its object. */
static void manager_gone(struct wire_resource *r)
{
    for (int i = 0; i < nshortcuts;)
        if (shortcuts[i].owner == r)
            shortcuts[i] = shortcuts[--nshortcuts];
        else
            i++;
}

static void bind_manager(struct wire_client *c, void *data, uint32_t version, uint32_t id)
{
    struct wire_resource *r = wire_resource_create(c, &session_manager_interface, (int)version, id);
    if (!r)
        return;
    struct client *cl = wire_client_get_user_data(c);
    if (!cl || (cl->uid != 0 && cl->uid != getuid())) {
        wire_client_post_error(c, r, 41, "the session manager requires root or the user of X12");
        return;
    }
    wire_resource_set_listener(r, &manager_handlers, NULL, manager_gone);
    session_manager_send_user(r, session_uid);
}

void session_init(struct wire_server *srv)
{
    server = srv;
    last_input = uptime_ms();
    wire_global_create(srv, &session_manager_interface, 1, bind_manager, NULL);
}

/* ---- events ---- */

int session_shortcut(uint32_t key, int mods)
{
    struct csurface *focus = seat_keyboard_focus();
    if (lock_active() || (focus && focus->role == ROLE_LAYER && focus->layer->layer == LAYER_OVERLAY))
        return 0;
    uint32_t want = (uint32_t)mods & (KEYMAP_MOD_SHIFT | KEYMAP_MOD_CTRL | KEYMAP_MOD_ALT | KEYMAP_MOD_LOGO);
    for (int i = 0; i < nshortcuts; i++)
        if (shortcuts[i].key == key && shortcuts[i].mods == want) {
            comp_log("shortcut %s", shortcuts[i].name);
            session_manager_send_shortcut(shortcuts[i].owner, shortcuts[i].name);
            return 1;
        }
    return 0;
}

static void send_locker_needed(struct wire_resource *r, const void *arg)
{
    (void)arg;
    session_manager_send_locker_needed(r);
}

void session_locker_needed(void)
{
    comp_log("a locker is needed");
    for_each_manager(send_locker_needed, NULL);
}

struct client_state {
    const struct client *c;
    int responding;
    const char *title;
};

static void send_client_state(struct wire_resource *r, const void *arg)
{
    const struct client_state *st = arg;
    session_manager_send_client_state(r, (uint32_t)st->c->number, st->c->pid > 0 ? (uint32_t)st->c->pid : 0,
                                      (uint32_t)st->responding, st->title);
}

void session_client_state(const struct client *c, int responding)
{
    /* The title of the topmost toplevel of the client. */
    const struct csurface *top = NULL;
    for (struct csurface *s = surface_first(); s; s = s->next)
        if (s->client == c && s->role == ROLE_TOPLEVEL && s->toplevel && (!top || s->stack > top->stack))
            top = s;
    struct client_state st = { c, responding, top ? top->toplevel->title : "" };
    for_each_manager(send_client_state, &st);
}

static void send_idle(struct wire_resource *r, const void *arg)
{
    (void)arg;
    session_manager_send_idle(r);
}

void session_note_input(void)
{
    last_input = uptime_ms();
    idle_sent = 0;
}

static int idle_counts(void)
{
    return settings.lock_timeout > 0 && !lock_active() && !idle_sent && session_uid >= 0;
}

long session_next_deadline(void)
{
    return idle_counts() ? last_input + (long)settings.lock_timeout * 1000 : -1;
}

void session_tick(long now)
{
    if (idle_counts() && now - last_input >= (long)settings.lock_timeout * 1000) {
        idle_sent = 1;
        comp_log("session idle for %d s", settings.lock_timeout);
        for_each_manager(send_idle, NULL);
    }
}
