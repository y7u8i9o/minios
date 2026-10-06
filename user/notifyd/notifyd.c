/* notifyd: the notification daemon of a graphical session
 * (docs/design/notifications.md).
 *
 * Programs post notifications over the "notify" protocol (protocol/
 * notify.xml), and the panel shows them through a notify_display object.
 * notifyd itself draws nothing. It owns the list of notifications, the
 * expiry of their pop-ups and the do-not-disturb state, and it delivers
 * actions to the programs that posted the notifications. The list is the
 * history of the session. A notification remains in the history after its
 * pop-up closes, until the user removes it or its sender closes it. The
 * history holds at most HISTORY_MAX entries.
 *
 * startgui starts notifyd with the session and restarts it if it fails.
 * With the option -q, notifyd is quiet. Otherwise it logs one line per
 * event on standard output, and the boot tests read these lines. */
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <limits.h>
#include <wire/server.h>
#include "notify-server.h"

#define NOTIFY_SOCKET "notify"
#define HISTORY_MAX 50
#define DEFAULT_TIMEOUT_MS 5000
#define MAX_ACTIONS 3

enum { URGENCY_LOW, URGENCY_NORMAL, URGENCY_CRITICAL };
enum { CLOSED_EXPIRED = 1, CLOSED_DISMISSED = 2, CLOSED_BY_CLIENT = 3, CLOSED_REMOVED = 4 };

/* One notification. The field owner is the notification object of the
 * program that posted it. It is NULL once that object is gone. */
struct note {
    uint32_t number;
    char app[64], summary[128], body[512], icon[64];
    unsigned urgency;
    time_t time;
    int nactions;
    char keys[MAX_ACTIONS][32], labels[MAX_ACTIONS][48];
    int popup;                          /* the pop-up is visible */
    uint64_t expires;                   /* monotonic milliseconds, 0 for none */
    struct wire_resource *owner;
    struct note *next;                  /* newest first */
};

/* The state of a notification object. Until the client sends show, the
 * structure collects the properties. Afterwards the field note points to
 * the notification that the object posted. */
struct request {
    struct note draft;
    int timeout;
    uint32_t replaces;
    int shown;
    struct note *note;                  /* NULL once the notification has closed */
};

struct display {
    struct wire_resource *resource;
    struct display *next;
};

static struct wire_server *server;
static struct note *notes;
static unsigned nnotes;
static uint32_t next_number = 1;
static struct display *displays;
static int dnd, quiet;

static void say(const char *fmt, ...)
{
    if (quiet)
        return;
    va_list ap;
    va_start(ap, fmt);
    printf("notifyd: ");
    vprintf(fmt, ap);
    printf("\n");
    fflush(stdout);
    va_end(ap);
}

static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

/* ---- telling the displays ---- */

static void send_note(struct wire_resource *r, const struct note *n)
{
    char actions[MAX_ACTIONS * 82] = "";
    for (int i = 0; i < n->nactions; i++) {
        strlcat(actions, n->keys[i], sizeof actions);
        strlcat(actions, "\n", sizeof actions);
        strlcat(actions, n->labels[i], sizeof actions);
        strlcat(actions, "\n", sizeof actions);
    }
    notify_display_send_notification(r, n->number, n->app, n->summary, n->body, n->icon, n->urgency,
                                     (uint32_t)n->time, actions);
}

static void broadcast_note(const struct note *n)
{
    for (struct display *d = displays; d; d = d->next)
        send_note(d->resource, n);
}

static void set_popup(struct note *n, int visible)
{
    if (n->popup == visible)
        return;
    n->popup = visible;
    if (!visible)
        n->expires = 0;
    for (struct display *d = displays; d; d = d->next)
        notify_display_send_popup(d->resource, n->number, (uint32_t)visible);
}

/* ---- the notification list ---- */

static struct note *find(uint32_t number)
{
    for (struct note *n = notes; n; n = n->next)
        if (n->number == number)
            return n;
    return NULL;
}

/* Tell the sender that the notification has closed. */
static void tell_closed(struct note *n, unsigned reason)
{
    if (n->owner) {
        notification_send_closed(n->owner, reason);
        struct request *rq = n->owner->data;
        if (rq)
            rq->note = NULL;
        n->owner = NULL;
    }
}

static void remove_note(struct note *n, unsigned reason)
{
    for (struct note **p = &notes; *p; p = &(*p)->next)
        if (*p == n) {
            *p = n->next;
            break;
        }
    nnotes--;
    tell_closed(n, reason);
    for (struct display *d = displays; d; d = d->next)
        notify_display_send_removed(d->resource, n->number);
    say("removed %u", n->number);
    free(n);
}

/* Return the timeout of a request in milliseconds, or 0 for no timeout. */
static int effective_timeout(const struct request *rq)
{
    if (rq->timeout >= 0)
        return rq->timeout;
    return rq->draft.urgency == URGENCY_CRITICAL ? 0 : DEFAULT_TIMEOUT_MS;
}

static void post(struct request *rq, struct wire_resource *owner)
{
    struct note *n = rq->replaces ? find(rq->replaces) : NULL;
    int replaced = n != NULL;
    if (n) {
        /* The replaced notification retains its number and its position in
         * the history. Its previous sender learns that the notification
         * closed. */
        if (n->owner != owner)
            tell_closed(n, CLOSED_BY_CLIENT);
        uint32_t number = n->number;
        struct note *next = n->next;
        int popup = n->popup;
        *n = rq->draft;
        n->number = number;
        n->next = next;
        n->popup = popup;
    } else {
        n = malloc(sizeof *n);
        if (!n)
            return;
        *n = rq->draft;
        n->number = next_number++;
        n->popup = 0;
        n->next = notes;
        notes = n;
        if (++nnotes > HISTORY_MAX) {
            struct note *oldest = notes;
            while (oldest->next)
                oldest = oldest->next;
            remove_note(oldest, CLOSED_REMOVED);
        }
    }
    n->time = time(NULL);
    n->owner = owner;
    rq->note = n;
    int timeout = effective_timeout(rq);
    broadcast_note(n);
    /* Do-not-disturb suppresses the pop-ups of all notifications except
     * critical ones. The suppressed notifications still enter the history. */
    if (!dnd || n->urgency == URGENCY_CRITICAL) {
        set_popup(n, 1);
        n->expires = timeout > 0 ? now_ms() + (uint64_t)timeout : 0;
    } else {
        set_popup(n, 0);
    }
    notification_send_shown(owner, n->number);
    say("%s %u '%s' from %s, urgency %u, timeout %d%s", replaced ? "replaced" : "shown", n->number, n->summary,
        n->app, n->urgency, timeout, n->popup ? "" : ", no pop-up");
}

/* ---- notification objects ---- */

static void handle_set_icon(struct wire_client *c, struct wire_resource *r, const char *icon)
{
    struct request *rq = r->data;
    strlcpy(rq->draft.icon, icon ? icon : "", sizeof rq->draft.icon);
}

static void handle_set_urgency(struct wire_client *c, struct wire_resource *r, uint32_t urgency)
{
    struct request *rq = r->data;
    rq->draft.urgency = urgency > URGENCY_CRITICAL ? URGENCY_CRITICAL : urgency;
}

static void handle_set_timeout(struct wire_client *c, struct wire_resource *r, int32_t timeout)
{
    struct request *rq = r->data;
    rq->timeout = timeout < 0 ? -1 : timeout;
}

static void handle_add_action(struct wire_client *c, struct wire_resource *r, const char *key, const char *label)
{
    struct request *rq = r->data;
    struct note *d = &rq->draft;
    if (d->nactions == MAX_ACTIONS || !key || !key[0] || strchr(key, '\n') || (label && strchr(label, '\n'))) {
        wire_client_post_error(c, r, 0, "invalid action");
        return;
    }
    strlcpy(d->keys[d->nactions], key, sizeof d->keys[0]);
    strlcpy(d->labels[d->nactions], label ? label : "", sizeof d->labels[0]);
    d->nactions++;
}

static void handle_set_replaces(struct wire_client *c, struct wire_resource *r, uint32_t number)
{
    struct request *rq = r->data;
    rq->replaces = number;
}

static void handle_show(struct wire_client *c, struct wire_resource *r)
{
    struct request *rq = r->data;
    if (rq->shown) {
        wire_client_post_error(c, r, 0, "notification already shown");
        return;
    }
    rq->shown = 1;
    post(rq, r);
}

static void handle_close(struct wire_client *c, struct wire_resource *r)
{
    struct request *rq = r->data;
    if (rq->note)
        remove_note(rq->note, CLOSED_BY_CLIENT);
}

static void handle_destroy(struct wire_client *c, struct wire_resource *r)
{
    wire_resource_destroy(r);
}

static const struct notification_impl notification_handlers = {
    .set_icon = handle_set_icon,
    .set_urgency = handle_set_urgency,
    .set_timeout = handle_set_timeout,
    .add_action = handle_add_action,
    .set_replaces = handle_set_replaces,
    .show = handle_show,
    .close = handle_close,
    .destroy = handle_destroy,
};

/* When its object goes away, the notification remains in the history. Only
 * the link to the sender ends. */
static void request_destroyed(struct wire_resource *r)
{
    struct request *rq = r->data;
    if (rq->note)
        rq->note->owner = NULL;
    free(rq);
}

/* ---- the manager and the displays ---- */

static void handle_create(struct wire_client *c, struct wire_resource *self, uint32_t id, const char *app,
                          const char *summary, const char *body)
{
    struct request *rq = calloc(1, sizeof *rq);
    struct wire_resource *r = rq ? wire_resource_create(c, &notification_interface, 1, id) : NULL;
    if (!r) {
        free(rq);
        return;
    }
    strlcpy(rq->draft.app, app ? app : "", sizeof rq->draft.app);
    strlcpy(rq->draft.summary, summary ? summary : "", sizeof rq->draft.summary);
    strlcpy(rq->draft.body, body ? body : "", sizeof rq->draft.body);
    rq->draft.urgency = URGENCY_NORMAL;
    rq->timeout = -1;
    wire_resource_set_listener(r, &notification_handlers, rq, request_destroyed);
}

static void handle_invoke(struct wire_client *c, struct wire_resource *r, uint32_t number, const char *key)
{
    struct note *n = find(number);
    if (!n)
        return;
    int known = 0;
    for (int i = 0; i < n->nactions; i++)
        known |= strcmp(n->keys[i], key) == 0;
    if (!known)
        return;
    say("action %u %s", number, key);
    if (n->owner)
        notification_send_action(n->owner, key);
    /* Invoking an action closes the pop-up, just as a dismissal does. */
    if (n->popup) {
        set_popup(n, 0);
        tell_closed(n, CLOSED_DISMISSED);
    }
}

static void handle_dismiss(struct wire_client *c, struct wire_resource *r, uint32_t number)
{
    struct note *n = find(number);
    if (!n || !n->popup)
        return;
    say("dismissed %u", number);
    set_popup(n, 0);
    tell_closed(n, CLOSED_DISMISSED);
}

static void handle_remove(struct wire_client *c, struct wire_resource *r, uint32_t number)
{
    struct note *n = find(number);
    if (n)
        remove_note(n, CLOSED_REMOVED);
}

static void handle_clear(struct wire_client *c, struct wire_resource *r)
{
    say("history cleared, %u entries", nnotes);
    while (notes)
        remove_note(notes, CLOSED_REMOVED);
}

static void handle_set_dnd(struct wire_client *c, struct wire_resource *r, uint32_t on)
{
    dnd = on != 0;
    say("do not disturb %s", dnd ? "on" : "off");
    if (dnd)
        for (struct note *n = notes; n; n = n->next)
            if (n->popup && n->urgency != URGENCY_CRITICAL) {
                set_popup(n, 0);
                tell_closed(n, CLOSED_DISMISSED);
            }
    for (struct display *d = displays; d; d = d->next)
        notify_display_send_dnd(d->resource, (uint32_t)dnd);
}

static const struct notify_display_impl display_handlers = {
    .invoke = handle_invoke,
    .dismiss = handle_dismiss,
    .remove = handle_remove,
    .clear = handle_clear,
    .set_dnd = handle_set_dnd,
    .destroy = handle_destroy,
};

static void display_destroyed(struct wire_resource *r)
{
    struct display *d = r->data;
    for (struct display **p = &displays; *p; p = &(*p)->next)
        if (*p == d) {
            *p = d->next;
            break;
        }
    free(d);
}

/* A new display receives the current state. It first receives the
 * do-not-disturb setting and then every notification, oldest first, with
 * the state of its pop-up. */
static void handle_get_display(struct wire_client *c, struct wire_resource *self, uint32_t id)
{
    struct display *d = calloc(1, sizeof *d);
    struct wire_resource *r = d ? wire_resource_create(c, &notify_display_interface, 1, id) : NULL;
    if (!r) {
        free(d);
        return;
    }
    d->resource = r;
    d->next = displays;
    displays = d;
    wire_resource_set_listener(r, &display_handlers, d, display_destroyed);
    notify_display_send_dnd(r, (uint32_t)dnd);
    struct note *order[HISTORY_MAX];
    unsigned count = 0;
    for (struct note *n = notes; n && count < HISTORY_MAX; n = n->next)
        order[count++] = n;
    while (count--) {
        send_note(r, order[count]);
        if (order[count]->popup)
            notify_display_send_popup(r, order[count]->number, 1);
    }
    say("display connected");
}

static const struct notify_manager_impl manager_handlers = {
    .create = handle_create,
    .get_display = handle_get_display,
};

static void manager_bind(struct wire_client *c, void *data, uint32_t version, uint32_t id)
{
    struct wire_resource *r = wire_resource_create(c, &notify_manager_interface, 1, id);
    if (r)
        wire_resource_set_listener(r, &manager_handlers, NULL, NULL);
}

/* ---- the loop ---- */

/* Close the pop-ups that have expired. Return the number of milliseconds
 * until the next pop-up expires, or -1 if none is waiting to expire. */
static int expire(void)
{
    uint64_t now = now_ms(), next = 0;
    for (struct note *n = notes; n; n = n->next) {
        if (!n->popup || !n->expires)
            continue;
        if (n->expires <= now) {
            say("expired %u", n->number);
            set_popup(n, 0);
            tell_closed(n, CLOSED_EXPIRED);
        } else if (!next || n->expires < next) {
            next = n->expires;
        }
    }
    if (!next)
        return -1;
    uint64_t wait = next - now;
    return wait > INT_MAX ? INT_MAX : (int)wait;
}

static volatile sig_atomic_t running = 1;

static void on_signal(int sig)
{
    running = 0;
}

int main(int argc, char **argv)
{
    int opt;
    while ((opt = getopt(argc, argv, "q")) != -1) {
        if (opt != 'q') {
            fprintf(stderr, "usage: notifyd [-q]\n");
            return 2;
        }
        quiet = 1;
    }
    signal(SIGTERM, on_signal);
    signal(SIGINT, on_signal);
    signal(SIGPIPE, SIG_IGN);
    server = wire_server_create(NOTIFY_SOCKET);
    if (!server) {
        perror("notifyd: socket");
        return 1;
    }
    if (!wire_global_create(server, &notify_manager_interface, 1, manager_bind, NULL)) {
        perror("notifyd: manager");
        wire_server_destroy(server);
        return 1;
    }
    say("started");
    while (running) {
        struct pollfd pf[OPEN_MAX];
        struct wire_client *client[OPEN_MAX];
        int n = 0, nclients = 0;
        pf[n++] = (struct pollfd){ wire_server_fd(server), POLLIN, 0 };
        for (struct wire_client *c = wire_server_first_client(server); c && n < OPEN_MAX; c = wire_client_next(c)) {
            client[nclients++] = c;
            pf[n++] = (struct pollfd){ wire_client_fd(c), POLLIN, 0 };
        }
        int r = poll(pf, (unsigned)n, expire());
        if (r < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        if (pf[0].revents & POLLIN)
            wire_server_accept(server);
        for (int i = 0; i < nclients; i++)
            if (pf[i + 1].revents & (POLLIN | POLLHUP))
                if (wire_client_dispatch(client[i]) < 0)
                    wire_client_destroy(client[i]);
        expire();
        for (struct wire_client *c = wire_server_first_client(server); c; c = wire_client_next(c))
            wire_client_flush(c);
    }
    wire_server_destroy(server);
    say("stopped");
    return 0;
}
