/* Server side of libwire: listening socket, clients, resources,
 * globals and the registry, request dispatch, delete_id. */
#include <wire/server.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>

extern const struct wire_interface display_interface;
extern const struct wire_interface registry_interface;
extern const struct wire_interface callback_interface;

struct wire_server {
    int listen_fd;
    struct wire_client *clients;
    struct wire_global *globals;
    uint32_t next_global, next_serial;
};

struct wire_client {
    struct wire_server *srv;
    struct wire_conn conn;
    struct wire_resource *resources;
    struct wire_resource *display;
    uint32_t next_server_id;
    void *data;
    wire_client_destroy_fn destroy;
    struct wire_client *next;
    int dead;
};

/* ---- server ---- */

struct wire_server *wire_server_create(const char *name)
{
    struct wire_server *s = calloc(1, sizeof *s);
    if (!s)
        return NULL;
    s->listen_fd = -1;
    s->next_global = 1;
    s->next_serial = 1;
    if (name) {
#ifdef MINIOS_HOST
        int fd = socket(AF_UNIX, SOCK_STREAM, 0);
#else
        int fd = socket(AF_UNIX, SOCK_STREAM, SOCK_CLOEXEC | SOCK_NONBLOCK);
#endif
        struct sockaddr_un addr;
    memset(&addr, 0, sizeof addr);
    addr.sun_family = AF_UNIX;
        strncpy(addr.sun_path, name, sizeof addr.sun_path - 1);
        if (fd < 0 || bind(fd, (struct sockaddr *)&addr, sizeof addr) < 0 || listen(fd, 8) < 0) {
            if (fd >= 0)
                close(fd);
            free(s);
            return NULL;
        }
        s->listen_fd = fd;
    }
    return s;
}

void wire_server_destroy(struct wire_server *s)
{
    while (s->clients)
        wire_client_destroy(s->clients);
    while (s->globals)
        wire_global_destroy(s, s->globals);
    if (s->listen_fd >= 0)
        close(s->listen_fd);
    free(s);
}

int wire_server_fd(struct wire_server *s) { return s->listen_fd; }
struct wire_client *wire_server_first_client(struct wire_server *s) { return s->clients; }
struct wire_client *wire_client_next(struct wire_client *c) { return c->next; }
uint32_t wire_server_next_serial(struct wire_server *s) { return s->next_serial++; }

/* ---- display and registry resources ---- */

static void display_sync(struct wire_client *c, struct wire_resource *display, uint32_t id)
{
    struct wire_resource *cb = wire_resource_create(c, &callback_interface, 1, id);
    if (!cb)
        return;
    union wire_arg args[1] = { { .u = wire_server_next_serial(c->srv) } };
    wire_resource_post(cb, 0, args);
    wire_resource_destroy(cb);
}

static void registry_bind(struct wire_client *c, struct wire_resource *registry, uint32_t name, const char *iface,
                          uint32_t version, uint32_t id)
{
    for (struct wire_global *g = c->srv->globals; g; g = g->next) {
        if (g->name != name)
            continue;
        if (strcmp(g->iface->name, iface) != 0 || version > (uint32_t)g->version) {
            wire_client_post_error(c, registry, 1, "bind: interface or version mismatch");
            return;
        }
        g->bind(c, g->data, version, id);
        return;
    }
    wire_client_post_error(c, registry, 1, "bind: unknown global");
}

static const struct { void (*bind)(struct wire_client *, struct wire_resource *, uint32_t, const char *, uint32_t, uint32_t); } registry_listener = { registry_bind };

static void send_global(struct wire_resource *registry, struct wire_global *g)
{
    union wire_arg args[3] = { { .u = g->name }, { .s = g->iface->name }, { .u = (uint32_t)g->version } };
    wire_resource_post(registry, 0, args);
}

static void display_get_registry(struct wire_client *c, struct wire_resource *display, uint32_t id)
{
    struct wire_resource *r = wire_resource_create(c, &registry_interface, 1, id);
    if (!r)
        return;
    wire_resource_set_listener(r, &registry_listener, NULL, NULL);
    for (struct wire_global *g = c->srv->globals; g; g = g->next)
        send_global(r, g);
}

static const struct { void (*sync)(struct wire_client *, struct wire_resource *, uint32_t);
                      void (*get_registry)(struct wire_client *, struct wire_resource *, uint32_t); } display_listener = { display_sync, display_get_registry };

/* ---- clients ---- */

struct wire_client *wire_server_add_client(struct wire_server *s, int fd)
{
    struct wire_client *c = calloc(1, sizeof *c);
    if (!c) {
        close(fd);
        return NULL;
    }
    c->srv = s;
    wire_conn_init(&c->conn, fd);
    c->next_server_id = WIRE_SERVER_ID_BASE;
    c->next = s->clients;
    s->clients = c;
    c->display = wire_resource_create(c, &display_interface, 1, 1);
    wire_resource_set_listener(c->display, &display_listener, NULL, NULL);
    return c;
}

struct wire_client *wire_server_accept(struct wire_server *s)
{
#ifdef MINIOS_HOST
    int fd = accept(s->listen_fd, NULL, NULL);
#else
    int fd = accept4(s->listen_fd, NULL, NULL, SOCK_CLOEXEC | SOCK_NONBLOCK);
#endif
    if (fd < 0)
        return NULL;
    return wire_server_add_client(s, fd);
}

void wire_client_destroy(struct wire_client *c)
{
    struct wire_server *s = c->srv;
    for (struct wire_client **pp = &s->clients; *pp; pp = &(*pp)->next)
        if (*pp == c) {
            *pp = c->next;
            break;
        }
    c->dead = 1;
    while (c->resources)
        wire_resource_destroy(c->resources);
    if (c->destroy)
        c->destroy(c, c->data);
    wire_conn_close(&c->conn);
    free(c);
}

int wire_client_fd(struct wire_client *c) { return c->conn.fd; }
int wire_client_flush(struct wire_client *c) { return wire_conn_flush(&c->conn); }
size_t wire_client_pending(struct wire_client *c) { return c->conn.out_len; }
void wire_client_set_user_data(struct wire_client *c, void *data, wire_client_destroy_fn destroy) { c->data = data; c->destroy = destroy; }
void *wire_client_get_user_data(struct wire_client *c) { return c->data; }
struct wire_server *wire_client_server(struct wire_client *c) { return c->srv; }

struct wire_resource *wire_client_find(struct wire_client *c, uint32_t id)
{
    for (struct wire_resource *r = c->resources; r; r = r->next)
        if (r->obj.id == id)
            return r;
    return NULL;
}

struct wire_resource *wire_client_first_resource(struct wire_client *c) { return c->resources; }

void wire_client_post_error(struct wire_client *c, struct wire_resource *r, uint32_t code, const char *message)
{
    union wire_arg args[3] = { { .u = r ? r->obj.id : 0 }, { .u = code }, { .s = message } };
    wire_resource_post(c->display, 0, args);
    wire_conn_flush(&c->conn);
    c->conn.error = 1;
}

/* ---- globals ---- */

struct wire_global *wire_global_create(struct wire_server *s, const struct wire_interface *iface, int version,
                                       wire_bind_fn bind, void *data)
{
    struct wire_global *g = calloc(1, sizeof *g);
    if (!g)
        return NULL;
    g->name = s->next_global++;
    g->iface = iface;
    g->version = version;
    g->bind = bind;
    g->data = data;
    g->next = s->globals;
    s->globals = g;
    /* Existing registries learn about it. */
    for (struct wire_client *c = s->clients; c; c = c->next)
        for (struct wire_resource *r = c->resources; r; r = r->next)
            if (r->obj.interface == &registry_interface)
                send_global(r, g);
    return g;
}

void wire_global_destroy(struct wire_server *s, struct wire_global *g)
{
    for (struct wire_global **pp = &s->globals; *pp; pp = &(*pp)->next)
        if (*pp == g) {
            *pp = g->next;
            break;
        }
    for (struct wire_client *c = s->clients; c; c = c->next)
        for (struct wire_resource *r = c->resources; r; r = r->next)
            if (r->obj.interface == &registry_interface) {
                union wire_arg args[1] = { { .u = g->name } };
                wire_resource_post(r, 1, args);
            }
    free(g);
}

/* ---- resources ---- */

struct wire_resource *wire_resource_create(struct wire_client *c, const struct wire_interface *iface, int version, uint32_t id)
{
    if (id == 0)
        id = c->next_server_id++;
    if (wire_client_find(c, id)) {
        wire_client_post_error(c, NULL, 2, "object id already in use");
        return NULL;
    }
    struct wire_resource *r = calloc(1, sizeof *r);
    if (!r)
        return NULL;
    r->obj.id = id;
    r->obj.interface = iface;
    r->obj.version = version;
    r->client = c;
    r->next = c->resources;
    c->resources = r;
    return r;
}

void wire_resource_set_listener(struct wire_resource *r, const void *listener, void *data, wire_resource_destroy_fn destroy)
{
    r->listener = listener;
    r->data = data;
    r->destroy = destroy;
}

void wire_resource_post(struct wire_resource *r, uint32_t opcode, const union wire_arg *args)
{
    if ((int)opcode >= r->obj.interface->nevents)
        return;
    wire_conn_marshal(&r->client->conn, r->obj.id, opcode, &r->obj.interface->events[opcode], args);
}

void wire_resource_destroy(struct wire_resource *r)
{
    struct wire_client *c = r->client;
    for (struct wire_resource **pp = &c->resources; *pp; pp = &(*pp)->next)
        if (*pp == r) {
            *pp = r->next;
            break;
        }
    if (r->destroy)
        r->destroy(r);
    if (r->obj.id < WIRE_SERVER_ID_BASE && r->obj.id != 1 && !c->dead) {
        union wire_arg args[1] = { { .u = r->obj.id } };
        wire_resource_post(c->display, 1, args);
    }
    free(r);
}

/* ---- dispatch ---- */

typedef void (*rfn0)(struct wire_client *, struct wire_resource *);
typedef void (*rfn1)(struct wire_client *, struct wire_resource *, uintptr_t);
typedef void (*rfn2)(struct wire_client *, struct wire_resource *, uintptr_t, uintptr_t);
typedef void (*rfn3)(struct wire_client *, struct wire_resource *, uintptr_t, uintptr_t, uintptr_t);
typedef void (*rfn4)(struct wire_client *, struct wire_resource *, uintptr_t, uintptr_t, uintptr_t, uintptr_t);
typedef void (*rfn5)(struct wire_client *, struct wire_resource *, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t);
typedef void (*rfn6)(struct wire_client *, struct wire_resource *, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t);
typedef void (*rfn7)(struct wire_client *, struct wire_resource *, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t);
typedef void (*rfn8)(struct wire_client *, struct wire_resource *, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t);
typedef void (*rfn9)(struct wire_client *, struct wire_resource *, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t);
typedef void (*rfn10)(struct wire_client *, struct wire_resource *, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t);
typedef void (*rfn11)(struct wire_client *, struct wire_resource *, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t);
typedef void (*rfn12)(struct wire_client *, struct wire_resource *, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t);

static void call(const void *listener, int index, struct wire_client *c, struct wire_resource *r, int n, const uintptr_t *v)
{
    void (*const *fns)(void) = listener;
    void (*f)(void) = fns[index];
    if (!f)
        return;
    switch (n) {
    case 0: ((rfn0)f)(c, r); break;
    case 1: ((rfn1)f)(c, r, v[0]); break;
    case 2: ((rfn2)f)(c, r, v[0], v[1]); break;
    case 3: ((rfn3)f)(c, r, v[0], v[1], v[2]); break;
    case 4: ((rfn4)f)(c, r, v[0], v[1], v[2], v[3]); break;
    case 5: ((rfn5)f)(c, r, v[0], v[1], v[2], v[3], v[4]); break;
    case 6: ((rfn6)f)(c, r, v[0], v[1], v[2], v[3], v[4], v[5]); break;
    case 7: ((rfn7)f)(c, r, v[0], v[1], v[2], v[3], v[4], v[5], v[6]); break;
    case 8: ((rfn8)f)(c, r, v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7]); break;
    case 9: ((rfn9)f)(c, r, v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7], v[8]); break;
    case 10: ((rfn10)f)(c, r, v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7], v[8], v[9]); break;
    case 11: ((rfn11)f)(c, r, v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7], v[8], v[9], v[10]); break;
    case 12: ((rfn12)f)(c, r, v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7], v[8], v[9], v[10], v[11]); break;
    }
}

static int dispatch_one(struct wire_client *c)
{
    uint32_t id, opcode;
    const uint8_t *body;
    size_t len;
    int r = wire_conn_peek(&c->conn, &id, &opcode, &body, &len);
    if (r <= 0)
        return r;
    struct wire_resource *res = wire_client_find(c, id);
    if (!res) {
        wire_client_post_error(c, NULL, 3, "request on an unknown object");
        return -1;
    }
    if ((int)opcode >= res->obj.interface->nrequests) {
        wire_client_post_error(c, res, 4, "unknown request");
        return -1;
    }
    const struct wire_message *m = &res->obj.interface->requests[opcode];
    union wire_arg args[WIRE_MAX_ARGS];
    struct wire_array arrays[WIRE_MAX_ARGS];
    if (wire_unmarshal(&c->conn, m, body, len, args, arrays) < 0) {
        wire_client_post_error(c, res, 5, "malformed request");
        return -1;
    }
    uintptr_t v[WIRE_MAX_ARGS];
    const char *sig = m->signature;
    for (int i = 0; i < m->nargs; i++) {
        int nullable = 0;
        if (*sig == '?') {
            nullable = 1;
            sig++;
        }
        char t = *sig++;
        switch (t) {
        case 'i': case 'f': v[i] = (uintptr_t)(intptr_t)args[i].i; break;
        case 'u': case 'n': v[i] = args[i].u; break;
        case 's': v[i] = (uintptr_t)args[i].s; break;
        case 'a': v[i] = (uintptr_t)args[i].a; break;
        case 'h': v[i] = (uintptr_t)(intptr_t)args[i].h; break;
        case 'o': {
            struct wire_resource *o = args[i].o ? wire_client_find(c, args[i].o) : NULL;
            if (args[i].o && !o) {
                wire_client_post_error(c, res, 6, "unknown object argument");
                return -1;
            }
            if (o && m->types[i] && strcmp(o->obj.interface->name, m->types[i]) != 0) {
                wire_client_post_error(c, res, 7, "object argument of the wrong interface");
                return -1;
            }
            (void)nullable;
            v[i] = (uintptr_t)o;
            break;
        }
        }
    }
    if (res->listener)
        call(res->listener, (int)opcode, c, res, m->nargs, v);
    /* A destructor request removes the object unless the handler did. */
    if (m->destructor && wire_client_find(c, id) == res)
        wire_resource_destroy(res);
    wire_conn_consume(&c->conn, len);
    return 1;
}

int wire_client_dispatch(struct wire_client *c)
{
    int r = wire_conn_read(&c->conn);
    if (r < 0 || (r == 0 && c->conn.error))
        return -1;
    int n = 0;
    while ((r = dispatch_one(c)) > 0)
        n++;
    if (r < 0 || c->conn.error)
        return -1;
    if (wire_conn_flush(&c->conn) < 0)
        return -1;
    return n;
}
