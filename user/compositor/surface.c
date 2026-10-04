/* Globals, clients, shared memory pools and buffers, surfaces with
 * pending and current state, commits, releases and frame callbacks. */
#include <sys/socket.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <limits.h>
#include "comp.h"

static struct csurface *surfaces;        /* newest first */
static int next_surface = 1, next_client = 1;

struct csurface *surface_first(void) { return surfaces; }

struct csurface *surface_by_resource(struct wire_resource *r)
{
    return r ? r->data : NULL;
}

void surface_unmap(struct csurface *s)
{
    if (s->mapped)
        scene_damage(decor_has(s) ? decor_extent(s) : surface_rect(s));
    s->mapped = 0;
}

/* ---- pools and buffers ---- */

static void pool_unref(struct pool *p)
{
    if (--p->refs > 0)
        return;
    if (p->map)
        munmap(p->map, p->size);
    close(p->fd);
    free(p);
}

static void pool_resource_destroy(struct wire_resource *r)
{
    struct pool *p = r->data;
    p->res = NULL;
    pool_unref(p);
}

static void buffer_resource_destroy(struct wire_resource *r)
{
    struct buffer *b = r->data;
    b->res = NULL;
    /* Surfaces still showing it retain the memory through the pool. */
    for (struct csurface *s = surfaces; s; s = s->next) {
        if (s->current.buffer == b) {
            s->current.buffer = NULL;
            s->mapped = 0;
            scene_damage((struct rect){ s->x, s->y, s->width, s->height });
        }
        if (s->pending.buffer == b) {
            s->pending.buffer = NULL;
            s->pending.has_buffer = 0;
        }
    }
    pool_unref(b->pool);
    free(b);
}

static void h_buffer_destroy(struct wire_client *c, struct wire_resource *self) { wire_resource_destroy(self); }
static const struct buffer_impl buffer_handlers = { h_buffer_destroy };

static void h_create_buffer(struct wire_client *c, struct wire_resource *self, uint32_t id, int32_t offset,
                            int32_t width, int32_t height, int32_t stride, uint32_t format)
{
    struct pool *p = self->data;
    if (width <= 0 || height <= 0 || width > INT_MAX / 4 || stride < width * 4 || offset < 0 ||
        (size_t)offset + (size_t)stride * (size_t)height > p->size ||
        (format != FORMAT_XRGB8888 && format != FORMAT_ARGB8888)) {
        wire_client_post_error(c, self, 10, "invalid buffer geometry or format");
        return;
    }
    struct buffer *b = calloc(1, sizeof *b);
    struct wire_resource *r = wire_resource_create(c, &buffer_interface, 1, id);
    if (!b || !r) {
        free(b);
        return;
    }
    b->res = r;
    b->pool = p;
    p->refs++;
    b->offset = offset;
    b->width = width;
    b->height = height;
    b->stride = stride;
    b->format = format;
    wire_resource_set_listener(r, &buffer_handlers, b, buffer_resource_destroy);
}

static void h_pool_resize(struct wire_client *c, struct wire_resource *self, int32_t size)
{
    struct pool *p = self->data;
    if (size <= (int32_t)p->size)
        return;
    uint8_t *map = mmap(NULL, (size_t)size, PROT_READ | PROT_WRITE, MAP_SHARED, p->fd, 0);
    if (map == MAP_FAILED) {
        wire_client_post_error(c, self, 11, "cannot map the resized pool");
        return;
    }
    munmap(p->map, p->size);
    p->map = map;
    p->size = (size_t)size;
}

static void h_pool_destroy(struct wire_client *c, struct wire_resource *self) { wire_resource_destroy(self); }
static const struct shm_pool_impl pool_handlers = { h_create_buffer, h_pool_resize, h_pool_destroy };

static void h_create_pool(struct wire_client *c, struct wire_resource *self, uint32_t id, int fd, int32_t size)
{
    struct pool *p = calloc(1, sizeof *p);
    struct wire_resource *r = p ? wire_resource_create(c, &shm_pool_interface, 1, id) : NULL;
    if (!r || size <= 0) {
        free(p);
        close(fd);
        return;
    }
    p->fd = fd;
    p->size = (size_t)size;
    p->refs = 1;
    p->res = r;
    p->map = mmap(NULL, p->size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (p->map == MAP_FAILED) {
        p->map = NULL;
        wire_client_post_error(c, self, 11, "cannot map the pool");
    }
    wire_resource_set_listener(r, &pool_handlers, p, pool_resource_destroy);
}
static const struct shm_impl shm_handlers = { h_create_pool };

static void bind_shm(struct wire_client *c, void *data, uint32_t version, uint32_t id)
{
    struct wire_resource *r = wire_resource_create(c, &shm_interface, (int)version, id);
    if (!r)
        return;
    wire_resource_set_listener(r, &shm_handlers, NULL, NULL);
    shm_send_format(r, FORMAT_XRGB8888);
    shm_send_format(r, FORMAT_ARGB8888);
}

/* ---- surfaces ---- */

static void surface_resource_destroy(struct wire_resource *r)
{
    struct csurface *s = r->data;
    for (struct csurface **pp = &surfaces; *pp; pp = &(*pp)->next)
        if (*pp == s) {
            *pp = s->next;
            break;
        }
    if (s->mapped)
        scene_damage(decor_has(s) ? decor_extent(s) : surface_rect(s));
    if (s->current.buffer)
        s->current.buffer->busy = 0;
    seat_surface_gone(s);
    data_surface_gone(s);
    shell_surface_gone(s);
    im_surface_gone(s);
    if (s->toplevel)
        s->toplevel->s = NULL;
    comp_log("surface %d destroyed", s->id);
    free(s);
}

static void h_attach(struct wire_client *c, struct wire_resource *self, struct wire_resource *buffer, int32_t x, int32_t y)
{
    struct csurface *s = self->data;
    s->pending.buffer = buffer ? buffer->data : NULL;
    s->pending.has_buffer = 1;
    s->pending.attach_x = x;
    s->pending.attach_y = y;
}

static void h_damage(struct wire_client *c, struct wire_resource *self, int32_t x, int32_t y, int32_t w, int32_t h)
{
    struct csurface *s = self->data;
    if (w <= 0 || h <= 0)
        return;
    struct rect r = { x, y, w, h };
    if (s->pending.ndamage == MAX_DAMAGE) {
        for (int i = 1; i < s->pending.ndamage; i++)
            s->pending.damage[0] = rect_union(s->pending.damage[0], s->pending.damage[i]);
        s->pending.ndamage = 1;
    }
    s->pending.damage[s->pending.ndamage++] = r;
}

static void h_frame(struct wire_client *c, struct wire_resource *self, uint32_t id)
{
    struct csurface *s = self->data;
    struct wire_resource *cb = wire_resource_create(c, &callback_interface, 1, id);
    if (!cb)
        return;
    if (s->pending.ncallbacks < 8)
        s->pending.callbacks[s->pending.ncallbacks++] = cb;
    else
        wire_resource_destroy(cb);
}

static int region_copy(struct rect out[MAX_REGION], const struct wire_array *a)
{
    if (!a || a->size % sizeof(struct rect) != 0)
        return -1;
    size_t n = a->size / sizeof(struct rect);
    if (n > MAX_REGION)
        n = MAX_REGION;
    const struct rect *in = a->data;
    int used = 0;
    for (size_t i = 0; i < n; i++) {
        if (in[i].w <= 0 || in[i].h <= 0)
            continue;
        out[used++] = in[i];
    }
    return used;
}

static void h_opaque(struct wire_client *c, struct wire_resource *self, const struct wire_array *rects)
{
    struct csurface *s = self->data;
    int n = region_copy(s->pending.opaque, rects);
    if (n < 0) {
        wire_client_post_error(c, self, 12, "malformed opaque region");
        return;
    }
    s->pending.nopaque = n;
    s->pending.opaque_set = 1;
}

static void h_input(struct wire_client *c, struct wire_resource *self, const struct wire_array *rects)
{
    struct csurface *s = self->data;
    int n = region_copy(s->pending.input, rects);
    if (n < 0) {
        wire_client_post_error(c, self, 12, "malformed input region");
        return;
    }
    s->pending.ninput = n;
    s->pending.input_set = 1;
}

static void begin_state(struct csurface *s)
{
    if (s->pending.state_set)
        return;
    s->pending.scale = s->current.scale > 0 ? s->current.scale : 1;
    s->pending.transform = s->current.transform;
    s->pending.state_set = 1;
}

static void h_scale(struct wire_client *c, struct wire_resource *self, int32_t scale)
{
    struct csurface *s = self->data;
    if (scale < 1 || scale > 8) {
        wire_client_post_error(c, self, 13, "invalid buffer scale");
        return;
    }
    begin_state(s);
    if (scale != s->pending.scale && (scale != 1 || s->current.scale != 1))
        comp_log("surface %d buffer scale %d", s->id, scale);
    s->pending.scale = scale;
}

static void h_transform(struct wire_client *c, struct wire_resource *self, uint32_t transform)
{
    struct csurface *s = self->data;
    if (transform > 3) {
        wire_client_post_error(c, self, 13, "invalid buffer transform");
        return;
    }
    begin_state(s);
    s->pending.transform = (int)transform;
}

static struct rect buffer_to_surface_damage(const struct csurface *s, struct rect r)
{
    int scale = s->pending.state_set ? s->pending.scale : s->current.scale;
    int transform = s->pending.state_set ? s->pending.transform : s->current.transform;
    if (scale < 1)
        scale = 1;
    int x0 = r.x / scale, y0 = r.y / scale;
    int x1 = (r.x + r.w + scale - 1) / scale;
    int y1 = (r.y + r.h + scale - 1) / scale;
    int bw = s->pending.buffer ? s->pending.buffer->width / scale : s->width;
    int bh = s->pending.buffer ? s->pending.buffer->height / scale : s->height;
    switch (transform) {
    case 1: return (struct rect){ bh - y1, x0, y1 - y0, x1 - x0 };
    case 2: return (struct rect){ bw - x1, bh - y1, x1 - x0, y1 - y0 };
    case 3: return (struct rect){ y0, bw - x1, y1 - y0, x1 - x0 };
    default: return (struct rect){ x0, y0, x1 - x0, y1 - y0 };
    }
}

static void h_damage_buffer(struct wire_client *c, struct wire_resource *self, int32_t x, int32_t y, int32_t w, int32_t h)
{
    if (w <= 0 || h <= 0)
        return;
    struct csurface *s = self->data;
    struct rect r = buffer_to_surface_damage(s, (struct rect){ x, y, w, h });
    h_damage(c, self, r.x, r.y, r.w, r.h);
}

static void h_commit(struct wire_client *c, struct wire_resource *self)
{
    struct csurface *s = self->data;
    struct buffer *next = s->pending.has_buffer ? s->pending.buffer : s->current.buffer;
    if (!surface_commit_allowed(c, s, next))
        return;
    struct rect old = decor_has(s) ? decor_extent(s) : surface_rect(s);
    if (s->pending.state_set) {
        s->current.scale = s->pending.scale;
        s->current.transform = s->pending.transform;
        s->pending.state_set = 0;
    }
    if (s->pending.opaque_set) {
        memcpy(s->current.opaque, s->pending.opaque, (size_t)s->pending.nopaque * sizeof s->current.opaque[0]);
        s->current.nopaque = s->pending.nopaque;
        s->current.opaque_set = 1;
        s->pending.opaque_set = 0;
    }
    if (s->pending.input_set) {
        memcpy(s->current.input, s->pending.input, (size_t)s->pending.ninput * sizeof s->current.input[0]);
        s->current.ninput = s->pending.ninput;
        s->current.input_set = 1;
        s->pending.input_set = 0;
    }
    if (s->pending.has_buffer) {
        struct buffer *prev = s->current.buffer, *b = s->pending.buffer;
        int was_mapped = s->mapped;
        if (prev && prev != b && prev->res) {
            prev->busy = 0;
            buffer_send_release(prev->res);
            comp_debug("buffer released");
        }
        s->current.buffer = b;
        if (b) {
            b->busy = 1;
            int scale = s->current.scale > 0 ? s->current.scale : 1;
            int bw = b->width / scale, bh = b->height / scale;
            s->width = s->current.transform & 1 ? bh : bw;
            s->height = s->current.transform & 1 ? bw : bh;
            if (s->role == ROLE_DND_ICON) {
                data_icon_committed(s, s->pending.attach_x, s->pending.attach_y);
            } else {
                s->x += s->pending.attach_x;
                s->y += s->pending.attach_y;
            }
            int first = !s->mapped;
            s->mapped = 1;
            if (first && s->role == ROLE_NONE)
                comp_log("surface %d mapped at %d,%d %dx%d", s->id, s->x, s->y, s->width, s->height);
            shell_surface_committed(s, first);
        } else {
            s->mapped = 0;
        }
        s->pending.has_buffer = 0;
        s->pending.buffer = NULL;
        s->pending.attach_x = s->pending.attach_y = 0;
        /* A new buffer for an unchanged geometry (the usual frame of a
         * double buffered client) changes the contents only; the
         * decorations and the shadow around them remain as drawn. */
        struct rect now = decor_has(s) ? decor_extent(s) : surface_rect(s);
        int same = was_mapped && b && old.x == now.x && old.y == now.y && old.w == now.w && old.h == now.h;
        if (same) {
            scene_damage(surface_rect(s));
        } else {
            scene_damage(old);
            scene_damage(now);
        }
    } else {
        for (int i = 0; i < s->pending.ndamage; i++) {
            struct rect d = s->pending.damage[i];
            scene_damage((struct rect){ s->x + d.x, s->y + d.y, d.w, d.h });
        }
    }
    s->pending.ndamage = 0;
    for (int i = 0; i < s->pending.ncallbacks && s->nframe_cbs < 8; i++)
        s->frame_cbs[s->nframe_cbs++] = s->pending.callbacks[i];
    s->pending.ncallbacks = 0;
    if (s->nframe_cbs && s->mapped)
        scene_damage(surface_rect(s));
    comp_debug("surface %d committed", s->id);
}

static void h_surface_destroy(struct wire_client *c, struct wire_resource *self) { wire_resource_destroy(self); }
static const struct surface_impl surface_handlers = {
    h_attach, h_damage, h_frame, h_opaque, h_commit, h_surface_destroy,
    h_input, h_scale, h_transform, h_damage_buffer,
};

static void h_create_surface(struct wire_client *c, struct wire_resource *self, uint32_t id)
{
    struct csurface *s = calloc(1, sizeof *s);
    struct wire_resource *r = s ? wire_resource_create(c, &surface_interface, 1, id) : NULL;
    if (!r) {
        free(s);
        return;
    }
    s->res = r;
    s->client = wire_client_get_user_data(c);
    s->id = next_surface++;
    s->current.scale = s->pending.scale = 1;
    /* Surfaces without a role are cascaded; roles reposition them. */
    int n = (s->id - 1) % 8;
    s->x = 40 + n * 30;
    s->y = 60 + n * 30;
    s->next = surfaces;
    surfaces = s;
    wire_resource_set_listener(r, &surface_handlers, s, surface_resource_destroy);
    comp_log("surface %d created for client %d", s->id, s->client ? s->client->number : 0);
}
static const struct compositor_impl compositor_handlers = { h_create_surface };

static void bind_compositor(struct wire_client *c, void *data, uint32_t version, uint32_t id)
{
    struct wire_resource *r = wire_resource_create(c, &compositor_interface, (int)version, id);
    if (r)
        wire_resource_set_listener(r, &compositor_handlers, NULL, NULL);
}

static void announce_output(struct wire_resource *r)
{
    output_send_geometry(r, 0, 0, screen_w, screen_h);
    output_send_mode(r, screen_w, screen_h, 60);
    output_send_scale(r, screen_scale);
    output_send_transform(r, 0);
    output_send_done(r);
}

static void bind_output(struct wire_client *c, void *data, uint32_t version, uint32_t id)
{
    struct wire_resource *r = wire_resource_create(c, &output_interface, (int)version, id);
    if (r)
        announce_output(r);
}

static struct wire_server *server;

void output_changed(void)
{
    for (struct wire_client *k = wire_server_first_client(server); k; k = wire_client_next(k))
        for (struct wire_resource *r = wire_client_first_resource(k); r; r = r->next)
            if (r->obj.interface == &output_interface)
                announce_output(r);
}

int surface_accepts_input(const struct csurface *s, int x, int y)
{
    int lx = x - s->x, ly = y - s->y;
    if (lx < 0 || ly < 0 || lx >= s->width || ly >= s->height)
        return 0;
    if (!s->current.input_set)
        return 1;
    for (int i = 0; i < s->current.ninput; i++)
        if (rect_contains(s->current.input[i], lx, ly))
            return 1;
    return 0;
}

/* ---- clients ---- */

static void client_gone(struct wire_client *wc, void *data)
{
    struct client *c = data;
    data_client_gone(c);
    text_client_gone(c);
    comp_log("client %d disconnected", c->number);
    trace_client(c, 0);
    free(c);
}

void surfaces_init(struct wire_server *srv)
{
    server = srv;
    wire_global_create(srv, &compositor_interface, 1, bind_compositor, NULL);
    wire_global_create(srv, &shm_interface, 1, bind_shm, NULL);
    wire_global_create(srv, &output_interface, 1, bind_output, NULL);
}

int session_uid = -1;

int client_uid_allowed(unsigned uid)
{
    return uid == 0 || uid == getuid() || (session_uid >= 0 && uid == (unsigned)session_uid);
}

void clients_drop_disallowed(void)
{
    for (;;) {
        struct wire_client *drop = NULL;
        for (struct wire_client *k = wire_server_first_client(server); k && !drop; k = wire_client_next(k)) {
            struct client *c = wire_client_get_user_data(k);
            if (c && !client_uid_allowed(c->uid))
                drop = k;
        }
        if (!drop)
            break;
        struct client *c = wire_client_get_user_data(drop);
        comp_log("client %d of uid %u disconnected", c->number, c->uid);
        wire_client_destroy(drop);
    }
}

void client_attach(struct wire_client *wc);
void client_attach(struct wire_client *wc)
{
    struct ucred peer = { 0 };
    socklen_t len = sizeof peer;
    if (getsockopt(wire_client_fd(wc), SOL_SOCKET, SO_PEERCRED, &peer, &len) < 0 || !client_uid_allowed(peer.uid)) {
        comp_log("client of uid %u refused", peer.uid);
        wire_client_destroy(wc);
        return;
    }
    struct client *c = calloc(1, sizeof *c);
    if (!c)
        return;
    c->wc = wc;
    c->uid = peer.uid;
    c->number = next_client++;
    wire_client_set_user_data(wc, c, client_gone);
    hang_client_attached(c);
    comp_log("client %d connected", c->number);
    trace_client(c, 1);
}

/* Frame callbacks of committed surfaces, sent after a composition. */
void surfaces_frame_done(uint32_t time_ms);
void surfaces_frame_done(uint32_t time_ms)
{
    for (struct csurface *s = surfaces; s; s = s->next) {
        for (int i = 0; i < s->nframe_cbs; i++) {
            callback_send_done(s->frame_cbs[i], time_ms);
            wire_resource_destroy(s->frame_cbs[i]);
        }
        s->nframe_cbs = 0;
    }
}
