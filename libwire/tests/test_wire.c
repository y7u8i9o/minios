/* Host tests of libwire over a socketpair: registry and bind, requests
 * with every argument type, descriptor passing, events, roundtrips,
 * delete_id and protocol errors. */
#include <wire/client.h>
#include <wire/server.h>
#include "core-client.h"
#include "core-server.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>

static int failures, checks;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* Server side state. */
static struct wire_server *srv;
static struct wire_client *cl;
static int surfaces_created, attaches, damages, commits, frames, pools, region_rects;
static int last_x, last_y, last_w, last_h, pool_fd = -1, pool_size, attach_x, attach_y;
static struct wire_resource *last_buffer, *last_surface, *frame_cb;
static char pool_text[32];

static void s_attach(struct wire_client *c, struct wire_resource *s, struct wire_resource *buffer, int32_t x, int32_t y)
{ attaches++; last_buffer = buffer; attach_x = x; attach_y = y; }
static void s_damage(struct wire_client *c, struct wire_resource *s, int32_t x, int32_t y, int32_t w, int32_t h)
{ damages++; last_x = x; last_y = y; last_w = w; last_h = h; }
static void s_frame(struct wire_client *c, struct wire_resource *s, uint32_t id)
{ frames++; frame_cb = wire_resource_create(c, &callback_interface, 1, id); }
static void s_region(struct wire_client *c, struct wire_resource *s, const struct wire_array *rects)
{ region_rects = (int)(rects->size / 16); }
static void s_commit(struct wire_client *c, struct wire_resource *s) { commits++; }
static void s_surface_destroy(struct wire_client *c, struct wire_resource *s) { wire_resource_destroy(s); }
static const struct surface_impl surface_handlers = { s_attach, s_damage, s_frame, s_region, s_commit, s_surface_destroy };

static void s_create_surface(struct wire_client *c, struct wire_resource *comp, uint32_t id)
{
    surfaces_created++;
    last_surface = wire_resource_create(c, &surface_interface, 1, id);
    wire_resource_set_listener(last_surface, &surface_handlers, NULL, NULL);
}
static const struct compositor_impl compositor_handlers = { s_create_surface };

static void bind_compositor(struct wire_client *c, void *data, uint32_t version, uint32_t id)
{
    struct wire_resource *r = wire_resource_create(c, &compositor_interface, (int)version, id);
    wire_resource_set_listener(r, &compositor_handlers, NULL, NULL);
}

static void s_create_buffer(struct wire_client *c, struct wire_resource *pool, uint32_t id, int32_t off, int32_t w, int32_t h, int32_t stride, uint32_t fmt)
{ wire_resource_create(c, &buffer_interface, 1, id); }
static void s_pool_resize(struct wire_client *c, struct wire_resource *pool, int32_t size) { pool_size = size; }
static void s_pool_destroy(struct wire_client *c, struct wire_resource *pool) { wire_resource_destroy(pool); }
static const struct shm_pool_impl pool_impl = { s_create_buffer, s_pool_resize, s_pool_destroy };

static void s_create_pool(struct wire_client *c, struct wire_resource *shm, uint32_t id, int fd, int32_t size)
{
    pools++;
    pool_fd = fd;
    pool_size = size;
    lseek(fd, 0, SEEK_SET);
    ssize_t n = read(fd, pool_text, sizeof pool_text - 1);
    pool_text[n > 0 ? n : 0] = '\0';
    struct wire_resource *r = wire_resource_create(c, &shm_pool_interface, 1, id);
    wire_resource_set_listener(r, &pool_impl, NULL, NULL);
}
static const struct shm_impl shm_handlers = { s_create_pool };

static void bind_shm(struct wire_client *c, void *data, uint32_t version, uint32_t id)
{
    struct wire_resource *r = wire_resource_create(c, &shm_interface, (int)version, id);
    wire_resource_set_listener(r, &shm_handlers, NULL, NULL);
    shm_send_format(r, 1);
}

/* Client side. */
static struct wire_proxy *compositor_proxy, *shm_proxy;
static int globals_seen, formats_seen, frame_done, releases;
static uint32_t frame_serial;

static void c_global(void *data, struct wire_proxy *registry, uint32_t name, const char *iface, uint32_t version)
{
    globals_seen++;
    if (strcmp(iface, "compositor") == 0)
        compositor_proxy = registry_bind(registry, name, iface, version, &compositor_interface, (int)version);
    else if (strcmp(iface, "shm") == 0)
        shm_proxy = registry_bind(registry, name, iface, version, &shm_interface, (int)version);
}
static void c_global_remove(void *data, struct wire_proxy *registry, uint32_t name) {}
static const struct registry_listener registry_impl = { c_global, c_global_remove };
static void c_format(void *data, struct wire_proxy *shm, uint32_t format) { formats_seen++; }
static const struct shm_listener shm_c_impl = { c_format };
static void c_done(void *data, struct wire_proxy *cb, uint32_t serial) { frame_done++; frame_serial = serial; wire_proxy_destroy(cb); }
static const struct callback_listener callback_impl = { c_done };
static void c_release(void *data, struct wire_proxy *b) { releases++; }
static const struct buffer_listener buffer_impl = { c_release };

/* Trace hook: each message becomes one line "> surface@3.attach(nil, -3, 7)"
 * for a request or "< callback@6.done(42)" for an event. */
static char trace_lines[64][160];
static int ntrace;
static size_t trace_bytes;

static const char *trace_object_name(void *data, uint32_t id)
{
    struct wire_resource *r = wire_client_find(data, id);
    return r ? r->obj.interface->name : NULL;
}

static void trace_hook(void *data, struct wire_client *c, int event, const struct wire_object *obj, uint32_t opcode,
                       const union wire_arg *args, size_t size)
{
    const struct wire_message *m = event ? &obj->interface->events[opcode] : &obj->interface->requests[opcode];
    char text[128];
    wire_format_args(text, sizeof text, m, args, trace_object_name, c);
    if (ntrace < 64)
        snprintf(trace_lines[ntrace++], sizeof trace_lines[0], "%c %s@%u.%s(%s)", event ? '<' : '>', obj->interface->name,
                 (unsigned)obj->id, m->name, text);
    trace_bytes += size;
}

static int traced(const char *line)
{
    for (int i = 0; i < ntrace; i++)
        if (strcmp(trace_lines[i], line) == 0)
            return 1;
    return 0;
}

/* Pump both sides until the client saw the given number of messages. */
static void pump(struct wire_display *d)
{
    wire_display_flush(d);
    for (int i = 0; i < 20; i++) {
        wire_client_dispatch(cl);
        wire_client_flush(cl);
        wire_display_dispatch(d);
    }
}

int main(void)
{
    int sv[2];
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0, "socketpair");
    /* Both ends non blocking so a dispatch with nothing pending returns. */
    fcntl(sv[0], F_SETFL, O_NONBLOCK);
    fcntl(sv[1], F_SETFL, O_NONBLOCK);
    srv = wire_server_create(NULL);
    wire_global_create(srv, &compositor_interface, 1, bind_compositor, NULL);
    wire_global_create(srv, &shm_interface, 1, bind_shm, NULL);
    cl = wire_server_add_client(srv, sv[0]);
    struct wire_display *d = wire_display_connect_fd(sv[1]);
    CHECK(d && cl, "connection objects");
    struct wire_proxy *registry = display_get_registry(wire_display_proxy(d));
    registry_add_listener(registry, &registry_impl, NULL);
    /* A roundtrip needs the server to respond in between. */
    wire_display_flush(d);
    wire_client_dispatch(cl);
    wire_client_flush(cl);
    wire_display_dispatch(d);
    CHECK(globals_seen == 2 && compositor_proxy && shm_proxy, "registry globals: %d", globals_seen);
    shm_add_listener(shm_proxy, &shm_c_impl, NULL);
    pump(d);
    CHECK(formats_seen == 1, "format event after bind: %d", formats_seen);
    /* A surface with every argument type, traced. */
    wire_server_set_trace(srv, trace_hook, NULL);
    struct wire_proxy *surface = compositor_create_surface(compositor_proxy);
    surface_attach(surface, NULL, -3, 7);
    surface_damage(surface, 1, 2, 300, 200);
    int32_t rects[8] = { 0, 0, 10, 10, 20, 20, 5, 5 };
    struct wire_array arr = { rects, sizeof rects };
    surface_set_opaque_region(surface, &arr);
    struct wire_proxy *cb = surface_frame(surface);
    callback_add_listener(cb, &callback_impl, NULL);
    surface_commit(surface);
    pump(d);
    CHECK(surfaces_created == 1 && attaches == 1 && last_buffer == NULL && attach_x == -3 && attach_y == 7, "attach with a null object and negative int");
    CHECK(damages == 1 && last_w == 300 && last_h == 200, "damage ints");
    CHECK(region_rects == 2, "array argument: %d rects", region_rects);
    CHECK(frames == 1 && commits == 1 && frame_cb, "frame and commit requests");
    CHECK(traced("> compositor@4.create_surface(new surface@5)"), "traced create_surface: %s", ntrace ? trace_lines[0] : "");
    CHECK(traced("> surface@5.attach(nil, -3, 7)"), "traced attach with a null object");
    CHECK(traced("> surface@5.set_opaque_region(array[32])"), "traced array argument");
    CHECK(traced("> surface@5.frame(new callback@6)"), "traced new id argument");
    CHECK(trace_bytes == 8 + 4 + 8 + 12 + 8 + 16 + 8 + 4 + 32 + 8 + 4 + 8, "traced sizes: %zu", trace_bytes);
    /* Server events: callback done, buffer release. */
    callback_send_done(frame_cb, 42);
    wire_resource_destroy(frame_cb);
    wire_client_flush(cl);
    wire_display_dispatch(d);
    CHECK(frame_done == 1 && frame_serial == 42, "done event with its argument");
    CHECK(traced("< callback@6.done(42)"), "traced event");
    CHECK(traced("< display@1.delete_id(6)"), "traced delete_id");
    wire_server_set_trace(srv, NULL, NULL);
    int before = ntrace;
    surface_commit(surface);
    pump(d);
    CHECK(ntrace == before, "no trace after the hook is removed");
    /* The formatter on its own: fixed point, escaped and cut strings,
     * descriptors, unknown objects. */
    static const char *const sample_types[6] = { NULL };
    const struct wire_message sample = { "sample", "f?sshou", 6, sample_types, 0 };
    char longer[100];
    memset(longer, 'x', sizeof longer - 1);
    longer[sizeof longer - 1] = '\0';
    union wire_arg sargs[6] = { { .i = -384 }, { .s = NULL }, { .s = "a\"b\n" }, { .h = 9 }, { .o = 77 }, { .u = 5 } };
    char text[200];
    wire_format_args(text, sizeof text, &sample, sargs, NULL, NULL);
    CHECK(strcmp(text, "-1.50, nil, \"a\\\"b\\x0a\", fd 9, @77, 5") == 0, "formatted arguments: %s", text);
    sargs[2].s = longer;
    size_t n = wire_format_args(text, 24, &sample, sargs, NULL, NULL);
    CHECK(n == 23 && strlen(text) == 23, "truncated text: %zu '%s'", n, text);
    /* An untyped new id is named by the string before it, as in registry.bind. */
    const struct wire_message bind = { "bind", "usun", 4, sample_types, 0 };
    union wire_arg bargs[4] = { { .u = 6 }, { .s = "seat" }, { .u = 1 }, { .n = 5 } };
    wire_format_args(text, sizeof text, &bind, bargs, NULL, NULL);
    CHECK(strcmp(text, "6, \"seat\", 1, new seat@5") == 0, "untyped new id: %s", text);
    /* Descriptor passing through create_pool. */
    char path[] = "/tmp/wiretestXXXXXX";
    int fd = mkstemp(path);
    write(fd, "pool contents", 13);
    struct wire_proxy *pool = shm_create_pool(shm_proxy, fd, 4096);
    struct wire_proxy *buffer = shm_pool_create_buffer(pool, 0, 32, 32, 128, 1);
    buffer_add_listener(buffer, &buffer_impl, NULL);
    surface_attach(surface, buffer, 0, 0);
    pump(d);
    CHECK(pools == 1 && pool_size == 4096 && strcmp(pool_text, "pool contents") == 0, "descriptor passed with the request: '%s'", pool_text);
    CHECK(pool_fd != fd, "the server got its own descriptor");
    CHECK(last_buffer && last_buffer->obj.interface == &buffer_interface, "object argument resolved to the buffer resource");
    buffer_send_release(last_buffer);
    wire_client_flush(cl);
    wire_display_dispatch(d);
    CHECK(releases == 1, "release event");
    /* Destructor requests free the id after delete_id. */
    uint32_t bid = wire_proxy_id(buffer);
    buffer_destroy(buffer);
    pump(d);
    CHECK(wire_client_find(cl, bid) == NULL, "buffer resource destroyed on the server");
    struct wire_proxy *again = compositor_create_surface(compositor_proxy);
    CHECK(wire_proxy_id(again) == bid, "freed id reused after delete_id: %u vs %u", wire_proxy_id(again), bid);
    pump(d);
    CHECK(surfaces_created == 2, "second surface");
    /* Roundtrip helper. */
    wire_display_flush(d);
    int rt = -2;
    for (int i = 0; i < 3 && rt == -2; i++) {
        wire_client_dispatch(cl);
        wire_client_flush(cl);
        /* wire_display_roundtrip blocks in dispatch; drive the server first. */
    }
    struct wire_proxy *cb2 = display_sync(wire_display_proxy(d));
    callback_add_listener(cb2, &callback_impl, NULL);
    pump(d);
    CHECK(frame_done == 2, "sync callback done");
    /* Protocol error: a request with a bad object id. */
    union wire_arg bad[3] = { { .o = 12345 }, { .i = 0 }, { .i = 0 } };
    wire_proxy_marshal(surface, 0, bad, NULL);
    pump(d);
    CHECK(wire_display_error(d) != 0, "protocol error reported to the client");
    wire_display_disconnect(d);
    wire_server_destroy(srv);
    close(fd);
    unlink(path);
    printf("wire tests: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
