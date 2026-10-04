/* Client side of libwire: the display connection, proxies with
 * listeners, dispatch of events, id allocation with delete_id. */
#include <wire/client.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>

#define MAX_OBJECTS 4096

struct wire_display {
    struct wire_conn conn;
    struct wire_proxy *objects[MAX_OBJECTS];      /* client ids 1.. */
    struct wire_proxy *server_objects[256];       /* WIRE_SERVER_ID_BASE.. */
    uint32_t next_id;
    uint32_t free_ids[256];
    int nfree;
    struct wire_proxy display;
    int error;
    int sync_done;
};

/* The display interface itself: sync(callback), get_registry(registry);
 * events error(object, code, message), delete_id(id). */
extern const struct wire_interface display_interface;
extern const struct wire_interface callback_interface;

static struct wire_proxy *lookup(struct wire_display *d, uint32_t id)
{
    if (id >= WIRE_SERVER_ID_BASE)
        return id - WIRE_SERVER_ID_BASE < 256 ? d->server_objects[id - WIRE_SERVER_ID_BASE] : NULL;
    return id < MAX_OBJECTS ? d->objects[id] : NULL;
}

static void store(struct wire_display *d, uint32_t id, struct wire_proxy *p)
{
    if (id >= WIRE_SERVER_ID_BASE) {
        if (id - WIRE_SERVER_ID_BASE < 256)
            d->server_objects[id - WIRE_SERVER_ID_BASE] = p;
    } else if (id < MAX_OBJECTS) {
        d->objects[id] = p;
    }
}

struct wire_display *wire_display_connect_fd(int fd)
{
    struct wire_display *d = calloc(1, sizeof *d);
    if (!d)
        return NULL;
    wire_conn_init(&d->conn, fd);
    d->next_id = 2;
    d->display.obj.id = 1;
    d->display.obj.interface = &display_interface;
    d->display.obj.version = 1;
    d->display.display = d;
    d->objects[1] = &d->display;
    return d;
}

struct wire_display *wire_display_connect(const char *name)
{
#ifdef MINIOS_HOST
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
#else
    int fd = socket(AF_UNIX, SOCK_STREAM, SOCK_CLOEXEC);
#endif
    if (fd < 0)
        return NULL;
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof addr);
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, name ? name : "display", sizeof addr.sun_path - 1);
    if (connect(fd, (struct sockaddr *)&addr, sizeof addr) < 0) {
        close(fd);
        return NULL;
    }
    return wire_display_connect_fd(fd);
}

void wire_display_disconnect(struct wire_display *d)
{
    wire_conn_flush(&d->conn);
    wire_conn_close(&d->conn);
    for (uint32_t i = 2; i < MAX_OBJECTS; i++)
        free(d->objects[i]);
    for (int i = 0; i < 256; i++)
        free(d->server_objects[i]);
    free(d);
}

int wire_display_fd(struct wire_display *d) { return d->conn.fd; }
struct wire_proxy *wire_display_proxy(struct wire_display *d) { return &d->display; }
int wire_display_flush(struct wire_display *d) { return wire_conn_flush(&d->conn); }
int wire_display_error(struct wire_display *d) { return d->error; }

/* ---- proxies ---- */

static uint32_t alloc_id(struct wire_display *d)
{
    if (d->nfree)
        return d->free_ids[--d->nfree];
    return d->next_id < MAX_OBJECTS ? d->next_id++ : 0;
}

struct wire_proxy *wire_proxy_create(struct wire_proxy *factory, const struct wire_interface *iface, int version)
{
    struct wire_display *d = factory->display;
    uint32_t id = alloc_id(d);
    if (!id)
        return NULL;
    struct wire_proxy *p = calloc(1, sizeof *p);
    if (!p)
        return NULL;
    p->obj.id = id;
    p->obj.interface = iface;
    p->obj.version = version;
    p->display = d;
    store(d, id, p);
    return p;
}

void wire_proxy_marshal(struct wire_proxy *p, uint32_t opcode, const union wire_arg *args, struct wire_proxy *created)
{
    struct wire_display *d = p->display;
    if ((int)opcode >= p->obj.interface->nrequests)
        return;
    if (wire_conn_marshal(&d->conn, p->obj.id, opcode, &p->obj.interface->requests[opcode], args) < 0)
        d->error = 1;
    (void)created;
}

void wire_proxy_destroy(struct wire_proxy *p)
{
    struct wire_display *d = p->display;
    if (p == &d->display)
        return;
    /* The id remains reserved until the server's delete_id arrives. */
    p->destroyed = 1;
    p->listener = NULL;
}

int wire_proxy_add_listener(struct wire_proxy *p, const void *listener, void *data)
{
    if (p->listener)
        return -1;
    p->listener = listener;
    p->data = data;
    return 0;
}

void wire_proxy_set_user_data(struct wire_proxy *p, void *data) { p->data = data; }
void *wire_proxy_get_user_data(struct wire_proxy *p) { return p->data; }

/* ---- dispatch ---- */

typedef void (*fn0)(void *, struct wire_proxy *);
typedef void (*fn1)(void *, struct wire_proxy *, uintptr_t);
typedef void (*fn2)(void *, struct wire_proxy *, uintptr_t, uintptr_t);
typedef void (*fn3)(void *, struct wire_proxy *, uintptr_t, uintptr_t, uintptr_t);
typedef void (*fn4)(void *, struct wire_proxy *, uintptr_t, uintptr_t, uintptr_t, uintptr_t);
typedef void (*fn5)(void *, struct wire_proxy *, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t);
typedef void (*fn6)(void *, struct wire_proxy *, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t);
typedef void (*fn7)(void *, struct wire_proxy *, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t);
typedef void (*fn8)(void *, struct wire_proxy *, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t);
typedef void (*fn9)(void *, struct wire_proxy *, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t);
typedef void (*fn10)(void *, struct wire_proxy *, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t);
typedef void (*fn11)(void *, struct wire_proxy *, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t);
typedef void (*fn12)(void *, struct wire_proxy *, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t);

static void call(const void *listener, int index, void *data, struct wire_proxy *p, int n, const uintptr_t *v)
{
    void (*const *fns)(void) = listener;
    void (*f)(void) = fns[index];
    if (!f)
        return;
    switch (n) {
    case 0: ((fn0)f)(data, p); break;
    case 1: ((fn1)f)(data, p, v[0]); break;
    case 2: ((fn2)f)(data, p, v[0], v[1]); break;
    case 3: ((fn3)f)(data, p, v[0], v[1], v[2]); break;
    case 4: ((fn4)f)(data, p, v[0], v[1], v[2], v[3]); break;
    case 5: ((fn5)f)(data, p, v[0], v[1], v[2], v[3], v[4]); break;
    case 6: ((fn6)f)(data, p, v[0], v[1], v[2], v[3], v[4], v[5]); break;
    case 7: ((fn7)f)(data, p, v[0], v[1], v[2], v[3], v[4], v[5], v[6]); break;
    case 8: ((fn8)f)(data, p, v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7]); break;
    case 9: ((fn9)f)(data, p, v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7], v[8]); break;
    case 10: ((fn10)f)(data, p, v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7], v[8], v[9]); break;
    case 11: ((fn11)f)(data, p, v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7], v[8], v[9], v[10]); break;
    case 12: ((fn12)f)(data, p, v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7], v[8], v[9], v[10], v[11]); break;
    }
}

static void free_id(struct wire_display *d, uint32_t id)
{
    struct wire_proxy *p = lookup(d, id);
    if (p && p != &d->display) {
        store(d, id, NULL);
        free(p);
    }
    if (id < WIRE_SERVER_ID_BASE && d->nfree < 256)
        d->free_ids[d->nfree++] = id;
}

static int dispatch_one(struct wire_display *d)
{
    uint32_t id, opcode;
    const uint8_t *body;
    size_t len;
    int r = wire_conn_peek(&d->conn, &id, &opcode, &body, &len);
    if (r <= 0)
        return r;
    struct wire_proxy *p = lookup(d, id);
    union wire_arg args[WIRE_MAX_ARGS];
    struct wire_array arrays[WIRE_MAX_ARGS];
    if (p && (int)opcode < p->obj.interface->nevents) {
        const struct wire_message *m = &p->obj.interface->events[opcode];
        if (wire_unmarshal(&d->conn, m, body, len, args, arrays) < 0) {
            d->error = 1;
            return -1;
        }
        if (p == &d->display) {
            if (opcode == 0) {              /* error */
                fprintf(stderr, "wire: protocol error on object %u code %u: %s\n", args[0].u, args[1].u,
                        args[2].s ? args[2].s : "");
                d->error = (int)args[1].u ? (int)args[1].u : -1;
            } else if (opcode == 1) {       /* delete_id */
                free_id(d, args[0].u);
            }
        } else if (p->listener && !p->destroyed) {
            uintptr_t v[WIRE_MAX_ARGS];
            const char *sig = m->signature;
            for (int i = 0; i < m->nargs; i++) {
                if (*sig == '?')
                    sig++;
                char t = *sig++;
                switch (t) {
                case 'i': case 'f': v[i] = (uintptr_t)(intptr_t)args[i].i; break;
                case 'u': v[i] = args[i].u; break;
                case 's': v[i] = (uintptr_t)args[i].s; break;
                case 'a': v[i] = (uintptr_t)args[i].a; break;
                case 'h': v[i] = (uintptr_t)(intptr_t)args[i].h; break;
                case 'o': v[i] = (uintptr_t)lookup(d, args[i].o); break;
                case 'n': {
                    /* A server created object: make its proxy. */
                    struct wire_proxy *np = calloc(1, sizeof *np);
                    if (np) {
                        np->obj.id = args[i].n;
                        np->obj.interface = NULL;
                        np->display = d;
                        store(d, args[i].n, np);
                    }
                    v[i] = (uintptr_t)np;
                    break;
                }
                }
            }
            call(p->listener, (int)opcode, p->data, p, m->nargs, v);
        } else {
            /* Descriptors of ignored events are closed by unmarshal's caller. */
            const char *sig = m->signature;
            for (int i = 0; i < m->nargs; i++) {
                if (*sig == '?') sig++;
                if (*sig++ == 'h')
                    close(args[i].h);
            }
        }
    }
    wire_conn_consume(&d->conn, len);
    return 1;
}

int wire_display_dispatch_pending(struct wire_display *d)
{
    int n = 0, r;
    while ((r = dispatch_one(d)) > 0)
        n++;
    return r < 0 ? -1 : n;
}

int wire_display_dispatch(struct wire_display *d)
{
    if (d->error && d->conn.in_len == 0)
        return -1;
    if (wire_conn_flush(&d->conn) < 0)
        return -1;
    int r = wire_conn_read(&d->conn);
    if (r < 0)
        return -1;
    if (r == 0 && d->conn.error)
        return -1;
    return wire_display_dispatch_pending(d);
}

static void sync_done(void *data, struct wire_proxy *cb, uint32_t serial)
{
    struct wire_display *d = data;
    d->sync_done = 1;
    wire_proxy_destroy(cb);
}

int wire_display_roundtrip(struct wire_display *d)
{
    static const struct { void (*done)(void *, struct wire_proxy *, uint32_t); } listener = { sync_done };
    struct wire_proxy *cb = wire_proxy_create(&d->display, &callback_interface, 1);
    if (!cb)
        return -1;
    union wire_arg args[1] = { { .n = cb->obj.id } };
    wire_proxy_add_listener(cb, &listener, d);
    wire_proxy_marshal(&d->display, 0, args, cb);
    d->sync_done = 0;
    while (!d->sync_done) {
        if (wire_display_dispatch(d) < 0)
            return -1;
    }
    return d->error ? -1 : 0;
}
