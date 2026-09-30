/* Protocol tracing for wireview (the tracer interface of
 * protocol/debug.xml). While a tracer is started, the libwire trace hook
 * turns every request X12 decodes and every event it queues into one
 * tracer.message event, with the arguments formatted as text. The traffic
 * of a client that traces is never traced, so that two tracers cannot
 * feed each other. A tracer whose socket backs up gets a count of the
 * messages it missed instead of the messages. X12 is single threaded, so
 * the tables below need no lock. */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "comp.h"

#define TRACERS_MAX 8
/* Bytes queued to a tracer beyond which its messages are only counted.
 * A message event is at most about 500 bytes, and the output buffer of
 * a connection holds 64 KiB, so a queued event is never dropped by libwire. */
#define TRACE_BACKLOG 49152

struct tracer {
    struct wire_resource *res;      /* NULL for a free slot */
    int active;
    uint32_t dropped;               /* messages not sent since the last dropped event */
};

static struct tracer tracers[TRACERS_MAX];
static int nactive;
static uint32_t seq;                /* number of the last traced message */
static int in_trace;                /* the hook is queueing its own events */
static struct wire_server *server;

static int client_traces(struct wire_client *c)
{
    for (int i = 0; i < TRACERS_MAX; i++)
        if (tracers[i].res && tracers[i].active && tracers[i].res->client == c)
            return 1;
    return 0;
}

static const char *object_name(void *data, uint32_t id)
{
    struct wire_resource *r = wire_client_find(data, id);
    return r ? r->obj.interface->name : NULL;
}

static void hook(void *data, struct wire_client *c, int event, const struct wire_object *obj, uint32_t opcode,
                 const union wire_arg *args, size_t size)
{
    if (in_trace || client_traces(c))
        return;
    const struct wire_message *m = event ? &obj->interface->events[opcode] : &obj->interface->requests[opcode];
    char text[256];
    wire_format_args(text, sizeof text, m, args, object_name, c);
    struct client *cl = wire_client_get_user_data(c);
    uint32_t number = cl ? (uint32_t)cl->number : 0;
    uint32_t n = ++seq;
    uint32_t now = (uint32_t)uptime_ms();
    in_trace = 1;
    for (int i = 0; i < TRACERS_MAX; i++) {
        struct tracer *t = &tracers[i];
        if (!t->res || !t->active)
            continue;
        if (wire_client_pending(t->res->client) > TRACE_BACKLOG) {
            t->dropped++;
            continue;
        }
        if (t->dropped) {
            tracer_send_dropped(t->res, t->dropped);
            t->dropped = 0;
        }
        tracer_send_message(t->res, n, now, number, (uint32_t)event, obj->id, obj->interface->name, m->name, text,
                           (uint32_t)size);
    }
    in_trace = 0;
}

static void set_hook(void)
{
    wire_server_set_trace(server, nactive ? hook : NULL, NULL);
}

static void send_client(struct tracer *t, const struct client *c, int connected)
{
    tracer_send_client(t->res, (uint32_t)c->number, (uint32_t)(c->pid > 0 ? c->pid : 0), (uint32_t)connected);
}

void trace_client(const struct client *c, int connected)
{
    if (!nactive || client_traces(c->wc))
        return;
    in_trace = 1;
    for (int i = 0; i < TRACERS_MAX; i++)
        if (tracers[i].res && tracers[i].active)
            send_client(&tracers[i], c, connected);
    in_trace = 0;
}

static void h_start(struct wire_client *c, struct wire_resource *self)
{
    struct tracer *t = self->data;
    if (t->active)
        return;
    t->active = 1;
    t->dropped = 0;
    nactive++;
    set_hook();
    struct client *own = wire_client_get_user_data(c);
    comp_log("client %d started a protocol trace", own ? own->number : 0);
    in_trace = 1;
    for (struct wire_client *k = wire_server_first_client(server); k; k = wire_client_next(k)) {
        struct client *cl = wire_client_get_user_data(k);
        if (cl && !client_traces(k))
            send_client(t, cl, 1);
    }
    in_trace = 0;
}

static void stop(struct tracer *t)
{
    if (!t->active)
        return;
    t->active = 0;
    nactive--;
    set_hook();
    if (t->dropped) {
        in_trace = 1;
        tracer_send_dropped(t->res, t->dropped);
        in_trace = 0;
        t->dropped = 0;
    }
}

static void h_stop(struct wire_client *c, struct wire_resource *self) { stop(self->data); }
static void h_destroy(struct wire_client *c, struct wire_resource *self) { wire_resource_destroy(self); }
static const struct tracer_impl tracer_handlers = { h_start, h_stop, h_destroy };

static void tracer_gone(struct wire_resource *r)
{
    struct tracer *t = r->data;
    t->dropped = 0;
    stop(t);
    t->res = NULL;
}

static void bind_tracer(struct wire_client *c, void *data, uint32_t version, uint32_t id)
{
    struct tracer *t = NULL;
    for (int i = 0; i < TRACERS_MAX && !t; i++)
        if (!tracers[i].res)
            t = &tracers[i];
    if (!t) {
        wire_client_post_error(c, NULL, 0, "too many tracers");
        return;
    }
    struct wire_resource *r = wire_resource_create(c, &tracer_interface, (int)version, id);
    if (!r)
        return;
    memset(t, 0, sizeof *t);
    t->res = r;
    wire_resource_set_listener(r, &tracer_handlers, t, tracer_gone);
}

void trace_init(struct wire_server *srv)
{
    server = srv;
    wire_global_create(srv, &tracer_interface, 1, bind_tracer, NULL);
}
