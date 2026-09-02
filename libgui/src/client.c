/* Window client over libwire: toplevel surfaces with two shared memory
 * buffers, damage committed once per compositor frame, input events
 * translated with the seat's keymap, the clipboard through the data
 * device. */
#include <gui/client.h>
#include <gui/keymap.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/ipc.h>
#include <wire/client.h>
#include "core-client.h"
#include "shell-client.h"
#include "seat-client.h"
#include "data-client.h"
#include "text-client.h"

#define QUEUE_MAX 256

struct wbuf {
    struct wire_proxy *proxy;
    int busy;                   /* held by the compositor */
    struct rect stale;          /* damage not yet copied into it */
    int has_stale;
};

struct win {
    struct gui_window *w;
    struct wire_proxy *surface, *toplevel, *popup, *layer;
    struct wire_proxy *pool;
    int fd;
    uint8_t *map;
    size_t map_size;
    int buf_w, buf_h;           /* size the pool holds */
    struct wbuf bufs[2];
    struct rect damage;
    int has_damage, frame_pending, need_commit;
    int min_w, min_h;
    int buttons;
    struct wire_proxy *old_pool, *old_bufs[2];
    uint8_t *old_map;
    size_t old_map_size;
    int old_fd;
    int px, py;                 /* last pointer position */
};

static struct wire_display *display;
static struct wire_proxy *compositor, *shm, *shell, *seat, *pointer, *keyboard, *data_manager, *data_device;
static struct wire_proxy *text_manager, *text_input;
static struct gui_window *wins;
static int screen_w = 1024, screen_h = 768;
struct output_state {
    struct wire_proxy *proxy;
    uint32_t name;
    int active;
    struct gui_output_info info;
};
static struct output_state outputs[8];
static int noutputs;
static struct wmsg queue[QUEUE_MAX];
static int qhead, qtail;
static struct keymap *keymap;
static int modifiers;
static uint32_t last_serial;
static struct gui_window *pointer_win, *keyboard_win;
static struct gui_window *text_win;
static int text_active;
static uint32_t text_serial;
static int next_id = 1;
static struct wire_proxy *selection_offer, *selection_source;
static char *clip_text;
static int clip_len;
static char offer_mime[64];

static void push(const struct wmsg *m)
{
    int next = (qtail + 1) % QUEUE_MAX;
    if (next == qhead)
        return;
    queue[qtail] = *m;
    qtail = next;
}

static struct gui_window *window_of_surface(struct wire_proxy *s)
{
    for (struct gui_window *w = wins; w; w = w->next)
        if (((struct win *)w->priv)->surface == s)
            return w;
    return NULL;
}

static struct gui_window *window_of_toplevel(struct wire_proxy *t)
{
    for (struct gui_window *w = wins; w; w = w->next)
        if (((struct win *)w->priv)->toplevel == t)
            return w;
    return NULL;
}

/* ---- registry and seat ---- */

/* Every global seen, for gui_bind_global. */
static struct { uint32_t name; char iface[32]; uint32_t version; } globals[32];
static int nglobals;
static struct wire_proxy *registry_proxy;

static void output_bounds(void)
{
    if (!noutputs)
        return;
    int first = -1;
    for (int i = 0; i < 8; i++) if (outputs[i].active) { first = i; break; }
    if (first < 0) return;
    int x0 = outputs[first].info.x, y0 = outputs[first].info.y;
    int x1 = x0 + outputs[first].info.width, y1 = y0 + outputs[first].info.height;
    for (int i = first + 1; i < 8; i++) {
        if (!outputs[i].active) continue;
        if (outputs[i].info.x < x0) x0 = outputs[i].info.x;
        if (outputs[i].info.y < y0) y0 = outputs[i].info.y;
        if (outputs[i].info.x + outputs[i].info.width > x1) x1 = outputs[i].info.x + outputs[i].info.width;
        if (outputs[i].info.y + outputs[i].info.height > y1) y1 = outputs[i].info.y + outputs[i].info.height;
    }
    screen_w = x1 - x0;
    screen_h = y1 - y0;
}

static void on_global(void *user, struct wire_proxy *registry, uint32_t name, const char *iface, uint32_t version)
{
    if (nglobals < 32) {
        globals[nglobals].name = name;
        strlcpy(globals[nglobals].iface, iface, sizeof globals[0].iface);
        globals[nglobals].version = version;
        nglobals++;
    }
    if (strcmp(iface, "compositor") == 0) compositor = registry_bind(registry, name, iface, version, &compositor_interface, 1);
    else if (strcmp(iface, "shm") == 0) shm = registry_bind(registry, name, iface, version, &shm_interface, 1);
    else if (strcmp(iface, "shell") == 0) shell = registry_bind(registry, name, iface, version, &shell_interface, 1);
    else if (strcmp(iface, "seat") == 0) seat = registry_bind(registry, name, iface, version, &seat_interface, 1);
    else if (strcmp(iface, "data_device_manager") == 0) data_manager = registry_bind(registry, name, iface, version, &data_device_manager_interface, 1);
    else if (strcmp(iface, "text_input_manager") == 0) text_manager = registry_bind(registry, name, iface, version, &text_input_manager_interface, 1);
    else if (strcmp(iface, "output") == 0) {
        extern const struct output_listener output_events;
        if (noutputs < (int)(sizeof outputs / sizeof outputs[0])) {
            struct output_state *o = NULL;
            for (int i = 0; i < 8; i++) if (!outputs[i].active) { o = &outputs[i]; break; }
            if (!o) return;
            memset(o, 0, sizeof *o);
            o->active = 1;
            noutputs++;
            o->name = name;
            o->info.scale = 1;
            o->proxy = registry_bind(registry, name, iface, version, &output_interface, 1);
            output_add_listener(o->proxy, &output_events, o);
        }
    }
}
static void on_global_remove(void *user, struct wire_proxy *registry, uint32_t name)
{
    for (int i = 0; i < 8; i++)
        if (outputs[i].active && outputs[i].name == name) {
            wire_proxy_destroy(outputs[i].proxy);
            memset(&outputs[i], 0, sizeof outputs[i]);
            noutputs--;
            output_bounds();
            break;
        }
}
static const struct registry_listener registry_events = { on_global, on_global_remove };
static void on_geometry(void *user, struct wire_proxy *o, int32_t x, int32_t y, int32_t w, int32_t h)
{ struct output_state *s = user; s->info.x = x; s->info.y = y; s->info.width = w; s->info.height = h; }
static void on_mode(void *user, struct wire_proxy *o, int32_t w, int32_t h, int32_t r)
{ struct output_state *s = user; s->info.width = w; s->info.height = h; s->info.refresh_hz = r; }
static void on_output_scale(void *user, struct wire_proxy *o, int32_t factor)
{ ((struct output_state *)user)->info.scale = factor > 0 ? factor : 1; }
static void on_output_transform(void *user, struct wire_proxy *o, uint32_t transform)
{ ((struct output_state *)user)->info.transform = (int)transform; }
/* The scale of the output windows live on; windows are re-created at
 * the new scale when it changes. */
static int output_scale(void)
{
    for (int i = 0; i < 8; i++)
        if (outputs[i].active)
            return outputs[i].info.scale > 0 ? outputs[i].info.scale : 1;
    return 1;
}
static void surface_resize(struct gui_window *w, int width, int height);
static void on_output_done(void *user, struct wire_proxy *o)
{
    output_bounds();
    int s = output_scale();
    for (struct gui_window *w = wins; w; w = w->next) {
        if (w->scale == s || !w->surf.pixels)
            continue;
        surface_resize(w, w->width, w->height);
        struct wmsg m = { WM_RESIZED, 0, w->id, w->width, w->height, 0, 0, "" };
        push(&m);
    }
}
const struct output_listener output_events = { on_geometry, on_mode, on_output_scale, on_output_transform, on_output_done };

static void on_ptr_enter(void *user, struct wire_proxy *p, uint32_t serial, struct wire_proxy *s, int32_t x, int32_t y)
{
    last_serial = serial;
    pointer_win = window_of_surface(s);
    if (pointer_win) {
        struct win *wi = pointer_win->priv;
        wi->px = wire_fixed_to_int(x);
        wi->py = wire_fixed_to_int(y);
        struct wmsg m = { WM_MOUSE, 0, pointer_win->id, wi->px, wi->py, wi->buttons, WMOUSE_MOVE, "" };
        push(&m);
    }
}
static void on_ptr_leave(void *user, struct wire_proxy *p, uint32_t serial, struct wire_proxy *s)
{
    if (pointer_win == window_of_surface(s))
        pointer_win = NULL;
}
static void on_ptr_motion(void *user, struct wire_proxy *p, uint32_t time, int32_t x, int32_t y)
{
    if (!pointer_win)
        return;
    struct win *wi = pointer_win->priv;
    wi->px = wire_fixed_to_int(x);
    wi->py = wire_fixed_to_int(y);
    struct wmsg m = { WM_MOUSE, 0, pointer_win->id, wi->px, wi->py, wi->buttons, WMOUSE_MOVE, "" };
    push(&m);
}
static void on_ptr_button(void *user, struct wire_proxy *p, uint32_t serial, uint32_t time, uint32_t button, uint32_t state)
{
    last_serial = serial;
    if (!pointer_win)
        return;
    struct win *wi = pointer_win->priv;
    int bit = 1 << (button - 1);
    wi->buttons = state ? wi->buttons | bit : wi->buttons & ~bit;
    struct wmsg m = { WM_MOUSE, 0, pointer_win->id, wi->px, wi->py, wi->buttons, state ? WMOUSE_DOWN : WMOUSE_UP, "" };
    push(&m);
}
static void on_ptr_axis(void *user, struct wire_proxy *p, uint32_t time, uint32_t axis, int32_t value)
{
    if (!pointer_win)
        return;
    struct win *wi = pointer_win->priv;
    struct wmsg m = { WM_MOUSE, 0, pointer_win->id, wi->px, wi->py, wire_fixed_to_int(value) / 15, WMOUSE_WHEEL, "" };
    push(&m);
}
static void on_ptr_frame(void *user, struct wire_proxy *p) {}
static const struct pointer_listener pointer_events = { on_ptr_enter, on_ptr_leave, on_ptr_motion, on_ptr_button, on_ptr_axis, on_ptr_frame };

static void on_keymap(void *user, struct wire_proxy *k, uint32_t format, int fd, uint32_t size)
{
    keymap_free(keymap);
    keymap = keymap_from_fd(fd, size);
    close(fd);
}
static void on_kbd_enter(void *user, struct wire_proxy *k, uint32_t serial, struct wire_proxy *s, const struct wire_array *keys)
{
    last_serial = serial;
    keyboard_win = window_of_surface(s);
    if (keyboard_win) {
        struct wmsg m = { WM_FOCUS, 0, keyboard_win->id, 1, 0, 0, 0, "" };
        push(&m);
    }
}
static void on_kbd_leave(void *user, struct wire_proxy *k, uint32_t serial, struct wire_proxy *s)
{
    struct gui_window *w = window_of_surface(s);
    if (w) {
        struct wmsg m = { WM_FOCUS, 0, w->id, 0, 0, 0, 0, "" };
        push(&m);
    }
    if (keyboard_win == w)
        keyboard_win = NULL;
}
static void on_key(void *user, struct wire_proxy *k, uint32_t serial, uint32_t time, uint32_t key, uint32_t state)
{
    last_serial = serial;
    if (!keyboard_win)
        return;
    int ch = keymap_translate(keymap, key, modifiers);
    if (keysym_is_symbol(ch))
        ch = 0;
    if (text_active && ch >= 32 && !(modifiers & (WMOD_CTRL | WMOD_ALT)))
        ch = 0;
    struct wmsg m = { WM_KEY, 0, keyboard_win->id, (int32_t)key, state ? 1 : 0, modifiers, ch, "" };
    push(&m);
}
static void on_modifiers(void *user, struct wire_proxy *k, uint32_t serial, uint32_t dep, uint32_t lat, uint32_t lock, uint32_t group)
{ modifiers = (int)dep; }
static void on_repeat(void *user, struct wire_proxy *k, int32_t rate, int32_t delay) {}
static const struct keyboard_listener keyboard_events = { on_keymap, on_kbd_enter, on_kbd_leave, on_key, on_modifiers, on_repeat };

/* ---- text input ---- */

static void on_text_enter(void *user, struct wire_proxy *ti, struct wire_proxy *surface)
{
    text_win = window_of_surface(surface);
    text_active = text_win != NULL;
}

static void on_text_leave(void *user, struct wire_proxy *ti, struct wire_proxy *surface)
{
    struct gui_window *w = window_of_surface(surface);
    if (text_win == w) {
        text_win = NULL;
        text_active = 0;
    }
}

static void push_text(uint32_t type, const char *text, int a, int b)
{
    if (!text_win)
        return;
    struct wmsg m = { type, 0, text_win->id, a, b, 0, 0, "" };
    strlcpy(m.text, text ? text : "", sizeof m.text);
    push(&m);
}

static void on_preedit(void *user, struct wire_proxy *ti, const char *text, int32_t begin, int32_t end)
{ push_text(WM_PREEDIT, text, begin, end); }
static void on_commit_string(void *user, struct wire_proxy *ti, const char *text)
{ push_text(WM_TEXT, text, 0, 0); }
static void on_delete_surrounding(void *user, struct wire_proxy *ti, uint32_t before, uint32_t after)
{ push_text(WM_TEXT_DELETE, "", (int)before, (int)after); }
static void on_text_done(void *user, struct wire_proxy *ti, uint32_t serial) {}
static const struct text_input_listener text_events = {
    on_text_enter, on_text_leave, on_preedit, on_commit_string, on_delete_surrounding, on_text_done,
};

/* ---- data device (clipboard) ---- */

static void on_src_send(void *user, struct wire_proxy *s, const char *mime, int fd)
{
    if (clip_text)
        write(fd, clip_text, (size_t)clip_len);
    close(fd);
}
static void on_src_cancelled(void *user, struct wire_proxy *s)
{
    if (selection_source == s)
        selection_source = NULL;
    wire_proxy_destroy(s);
}
static void on_src_drop(void *user, struct wire_proxy *s) {}
static void on_src_finished(void *user, struct wire_proxy *s) {}
static const struct data_source_listener source_events = { on_src_send, on_src_cancelled, on_src_drop, on_src_finished };
static void on_offer_mime(void *user, struct wire_proxy *o, const char *mime) { strlcpy(offer_mime, mime, sizeof offer_mime); }
static const struct data_offer_listener offer_events = { on_offer_mime };
static void on_dev_offer(void *user, struct wire_proxy *dev, struct wire_proxy *offer)
{
    offer->obj.interface = &data_offer_interface;
    data_offer_add_listener(offer, &offer_events, NULL);
}
static void on_dev_enter(void *user, struct wire_proxy *dev, uint32_t serial, struct wire_proxy *s, int32_t x, int32_t y, struct wire_proxy *offer)
{
    if (offer)
        data_offer_accept(offer, serial, offer_mime[0] ? offer_mime : "text/plain");
}
static void on_dev_leave(void *user, struct wire_proxy *dev) {}
static void on_dev_motion(void *user, struct wire_proxy *dev, uint32_t time, int32_t x, int32_t y) {}
static void on_dev_drop(void *user, struct wire_proxy *dev) {}
static void on_dev_selection(void *user, struct wire_proxy *dev, struct wire_proxy *offer)
{
    if (selection_offer && selection_offer != offer)
        data_offer_destroy(selection_offer);
    selection_offer = offer;
}
static const struct data_device_listener device_events = { on_dev_offer, on_dev_enter, on_dev_leave, on_dev_motion, on_dev_drop, on_dev_selection };

/* ---- connection ---- */

int gui_connect(void)
{
    signal(SIGPIPE, SIG_IGN);           /* a vanished compositor is reported as an error, not a signal */
    display = wire_display_connect(NULL);
    if (!display)
        return -1;
    fcntl(wire_display_fd(display), F_SETFL, O_NONBLOCK);
    struct wire_proxy *registry = display_get_registry(wire_display_proxy(display));
    registry_proxy = registry;
    registry_add_listener(registry, &registry_events, NULL);
    /* The socket is non blocking: poll before each blocking dispatch. */
    for (int i = 0; i < 2; i++) {
        wire_display_flush(display);
        struct pollfd pf = { wire_display_fd(display), POLLIN, 0 };
        poll(&pf, 1, 2000);
        if (wire_display_dispatch(display) < 0)
            return -1;
    }
    if (!compositor || !shm || !shell || !seat) {
        errno = ENOENT;
        return -1;
    }
    pointer = seat_get_pointer(seat);
    pointer_add_listener(pointer, &pointer_events, NULL);
    keyboard = seat_get_keyboard(seat);
    keyboard_add_listener(keyboard, &keyboard_events, NULL);
    if (text_manager) {
        text_input = text_input_manager_get_text_input(text_manager, seat);
        text_input_add_listener(text_input, &text_events, NULL);
    }
    if (data_manager) {
        data_device = data_device_manager_get_data_device(data_manager, seat);
        data_device_add_listener(data_device, &device_events, NULL);
    }
    wire_display_flush(display);
    return 0;
}

void gui_disconnect(void)
{
    if (!display)
        return;
    while (wins)
        gui_destroy_window(wins);
    wire_display_flush(display);
    wire_display_disconnect(display);
    display = NULL;
}

struct wire_display *gui_display(void) { return display; }

struct wire_proxy *gui_bind_global(const char *iface, const struct wire_interface *interface, int version)
{
    for (int i = 0; i < nglobals; i++)
        if (strcmp(globals[i].iface, iface) == 0)
            return registry_bind(registry_proxy, globals[i].name, iface, (uint32_t)version, interface, version);
    return NULL;
}

int gui_screen_width(void) { return screen_w; }
int gui_screen_height(void) { return screen_h; }
int gui_output_count(void) { return noutputs; }
int gui_get_output(int index, struct gui_output_info *out)
{
    if (index < 0 || index >= noutputs || !out)
        return -1;
    for (int i = 0; i < 8; i++)
        if (outputs[i].active && index-- == 0) {
            *out = outputs[i].info;
            return 0;
        }
    return -1;
}
int gui_event_fd(void) { return display ? wire_display_fd(display) : -1; }

/* ---- buffers ---- */

static void release_old(struct win *wi)
{
    for (int i = 0; i < 2; i++)
        if (wi->old_bufs[i]) {
            buffer_destroy(wi->old_bufs[i]);
            wi->old_bufs[i] = NULL;
        }
    if (wi->old_pool) {
        shm_pool_destroy(wi->old_pool);
        wi->old_pool = NULL;
    }
    if (wi->old_map) {
        munmap(wi->old_map, wi->old_map_size);
        wi->old_map = NULL;
    }
    if (wi->old_fd >= 0) {
        close(wi->old_fd);
        wi->old_fd = -1;
    }
}

static int pool_alloc(struct win *wi, int w, int h)
{
    size_t size = (size_t)w * h * 4 * 2;
    int fd = memfd_create("gui", MFD_CLOEXEC);
    if (fd < 0 || ftruncate(fd, (long)size) < 0)
        return -1;
    uint8_t *map = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (map == MAP_FAILED) {
        close(fd);
        return -1;
    }
    /* The previous pool stays until the new buffer is on screen, so
     * the compositor never sees the surface without a buffer. */
    release_old(wi);
    for (int i = 0; i < 2; i++) {
        wi->old_bufs[i] = wi->bufs[i].proxy;
        wi->bufs[i].proxy = NULL;
    }
    wi->old_pool = wi->pool;
    wi->old_map = wi->map;
    wi->old_map_size = wi->map_size;
    wi->old_fd = wi->fd;
    wi->fd = fd;
    wi->map = map;
    wi->map_size = size;
    wi->buf_w = w;
    wi->buf_h = h;
    wi->pool = shm_create_pool(shm, fd, (int32_t)size);
    for (int i = 0; i < 2; i++) {
        wi->bufs[i].proxy = shm_pool_create_buffer(wi->pool, i * w * h * 4, w, h, w * 4, 1);
        wire_proxy_set_user_data(wi->bufs[i].proxy, &wi->bufs[i]);
        extern const struct buffer_listener buffer_events;
        buffer_add_listener(wi->bufs[i].proxy, &buffer_events, &wi->bufs[i]);
        wi->bufs[i].busy = 0;
        wi->bufs[i].stale = (struct rect){ 0, 0, w, h };
        wi->bufs[i].has_stale = 1;
    }
    return 0;
}

static void on_release(void *user, struct wire_proxy *b)
{
    struct wbuf *wb = user;
    wb->busy = 0;
}
const struct buffer_listener buffer_events = { on_release };

/* Size the surface and the buffers for width by height logical pixels
 * at the output's scale. Damage is kept in device pixels. */
static void surface_resize(struct gui_window *w, int width, int height)
{
    struct win *wi = w->priv;
    int scale = output_scale();
    int dw = width * scale, dh = height * scale;
    uint32_t *px = malloc((size_t)dw * dh * 4);
    if (!px)
        return;
    memset(px, 0xdc, (size_t)dw * dh * 4);
    if (w->surf.pixels) {
        struct surface n = { px, dw, dh, dw };
        gfx_blit(&n, 0, 0, &w->surf, NULL);
        free(w->surf.pixels);
    }
    w->surf.pixels = px;
    w->surf.width = dw;
    w->surf.height = dh;
    w->surf.stride = dw;
    w->width = width;
    w->height = height;
    w->scale = scale;
    pool_alloc(wi, dw, dh);
    surface_set_buffer_scale(wi->surface, scale);
    struct rect opaque = { 0, 0, width, height };
    struct wire_array region = { &opaque, sizeof opaque };
    surface_set_opaque_region(wi->surface, &region);
    wi->damage = (struct rect){ 0, 0, dw, dh };
    wi->has_damage = 1;
}

static void commit_now(struct gui_window *w);

static void on_frame_done(void *user, struct wire_proxy *cb, uint32_t t)
{
    struct gui_window *w = user;
    struct win *wi = w->priv;
    wire_proxy_destroy(cb);
    wi->frame_pending = 0;
    if (wi->need_commit)
        commit_now(w);
}
static const struct callback_listener frame_events = { on_frame_done };

static void commit_now(struct gui_window *w)
{
    struct win *wi = w->priv;
    if (!wi->has_damage)
        return;
    struct wbuf *wb = NULL;
    for (int i = 0; i < 2; i++)
        if (!wi->bufs[i].busy) {
            wb = &wi->bufs[i];
            break;
        }
    if (!wb || wi->frame_pending) {
        wi->need_commit = 1;
        return;
    }
    /* Everything changed since this buffer was last shown. */
    for (int i = 0; i < 2; i++) {
        struct wbuf *o = &wi->bufs[i];
        o->stale = o->has_stale ? rect_union(o->stale, wi->damage) : wi->damage;
        o->has_stale = 1;
    }
    int index = (int)(wb - wi->bufs);
    struct surface dst = { (uint32_t *)(wi->map + (size_t)index * wi->buf_w * wi->buf_h * 4), wi->buf_w, wi->buf_h, wi->buf_w };
    struct rect r = rect_intersect(wb->stale, (struct rect){ 0, 0, w->surf.width, w->surf.height });
    gfx_copy_rect(&dst, &w->surf, &r);
    wb->has_stale = 0;
    wb->busy = 1;
    surface_attach(wi->surface, wb->proxy, 0, 0);
    /* The protocol takes surface (logical) coordinates: round outwards. */
    int s = w->scale > 0 ? w->scale : 1;
    int lx0 = r.x / s, ly0 = r.y / s, lx1 = (r.x + r.w + s - 1) / s, ly1 = (r.y + r.h + s - 1) / s;
    surface_damage(wi->surface, lx0, ly0, lx1 - lx0, ly1 - ly0);
    struct wire_proxy *cb = surface_frame(wi->surface);
    callback_add_listener(cb, &frame_events, w);
    surface_commit(wi->surface);
    release_old(wi);
    wi->frame_pending = 1;
    wi->has_damage = 0;
    wi->need_commit = 0;
}

void gui_flush(void)
{
    for (struct gui_window *w = wins; w; w = w->next)
        commit_now(w);
    if (display)
        wire_display_flush(display);
}

/* ---- windows ---- */

static void on_configure(void *user, struct wire_proxy *t, uint32_t serial, int32_t width, int32_t height, const struct wire_array *states)
{
    struct gui_window *w = window_of_toplevel(t);
    if (!w)
        return;
    struct win *wi = w->priv;
    toplevel_ack_configure(t, serial);
    if (width > 0 && height > 0 && (width != w->width || height != w->height)) {
        if (wi->min_w && width < wi->min_w) width = wi->min_w;
        if (wi->min_h && height < wi->min_h) height = wi->min_h;
        surface_resize(w, width, height);
        struct wmsg m = { WM_RESIZED, 0, w->id, width, height, 0, 0, "" };
        push(&m);
    }
}
static void on_close(void *user, struct wire_proxy *t)
{
    struct gui_window *w = window_of_toplevel(t);
    if (w) {
        struct wmsg m = { WM_CLOSE, 0, w->id, 0, 0, 0, 0, "" };
        push(&m);
    }
}
static const struct toplevel_listener toplevel_events = { on_configure, on_close };
static void on_enter_output(void *user, struct wire_proxy *s, struct wire_proxy *o) {}
static const struct surface_listener surface_events = { on_enter_output };

static void on_layer_configure(void *user, struct wire_proxy *l, uint32_t serial, int32_t width, int32_t height)
{
    struct gui_window *w = NULL;
    for (struct gui_window *k = wins; k; k = k->next)
        if (((struct win *)k->priv)->layer == l)
            w = k;
    if (!w)
        return;
    layer_surface_ack_configure(l, serial);
    if (width > 0 && height > 0 && (width != w->width || height != w->height)) {
        surface_resize(w, width, height);
        struct wmsg m = { WM_RESIZED, 0, w->id, width, height, 0, 0, "" };
        push(&m);
    }
}
static void on_layer_closed(void *user, struct wire_proxy *l)
{
    for (struct gui_window *k = wins; k; k = k->next)
        if (((struct win *)k->priv)->layer == l) {
            struct wmsg m = { WM_CLOSE, 0, k->id, 0, 0, 0, 0, "" };
            push(&m);
        }
}
static const struct layer_surface_listener layer_events = { on_layer_configure, on_layer_closed };

static struct gui_window *window_of_popup(struct wire_proxy *p)
{
    for (struct gui_window *w = wins; w; w = w->next)
        if (((struct win *)w->priv)->popup == p)
            return w;
    return NULL;
}

static void on_popup_configure(void *user, struct wire_proxy *p, uint32_t serial, int32_t x, int32_t y, int32_t width, int32_t height)
{
    struct gui_window *w = window_of_popup(p);
    if (!w)
        return;
    popup_ack_configure(p, serial);
    if (width > 0 && height > 0 && (width != w->width || height != w->height))
        surface_resize(w, width, height);
}

static void on_popup_done(void *user, struct wire_proxy *p)
{
    struct gui_window *w = window_of_popup(p);
    if (w) {
        struct wmsg m = { WM_CLOSE, 0, w->id, 0, 0, 0, 0, "" };
        push(&m);
    }
}
static const struct popup_listener popup_events = { on_popup_configure, on_popup_done };

static struct gui_window *window_alloc(void)
{
    struct gui_window *w = calloc(1, sizeof *w);
    struct win *wi = calloc(1, sizeof *wi);
    if (!w || !wi) {
        free(w);
        free(wi);
        return NULL;
    }
    wi->fd = -1;
    wi->old_fd = -1;
    wi->w = w;
    w->priv = wi;
    w->id = next_id++;
    wi->surface = compositor_create_surface(compositor);
    surface_add_listener(wi->surface, &surface_events, NULL);
    w->next = wins;
    wins = w;
    return w;
}

static void roundtrip(void);

struct gui_window *gui_create_layer_window(int width, int height, int layer, int anchor, int exclusive,
                                           int keyboard, const char *ns)
{
    if (!display)
        return NULL;
    struct gui_window *w = window_alloc();
    if (!w)
        return NULL;
    struct win *wi = w->priv;
    wi->layer = shell_get_layer_surface(shell, wi->surface, (uint32_t)layer, ns);
    layer_surface_add_listener(wi->layer, &layer_events, NULL);
    layer_surface_set_anchor(wi->layer, (uint32_t)anchor);
    layer_surface_set_exclusive_zone(wi->layer, exclusive);
    layer_surface_set_keyboard_interactivity(wi->layer, (uint32_t)keyboard);
    layer_surface_set_size(wi->layer, width, height);
    /* The compositor answers with the size (the free desktop area when
     * a dimension is 0); wait for it so the caller sees the real size. */
    roundtrip();
    if (w->width == 0)
        surface_resize(w, width > 0 ? width : screen_w, height > 0 ? height : screen_h);
    gui_flush();
    return w;
}

struct gui_window *gui_create_window(int width, int height, const char *title)
{
    if (!display)
        return NULL;
    struct gui_window *w = window_alloc();
    if (!w)
        return NULL;
    struct win *wi = w->priv;
    wi->toplevel = shell_get_toplevel(shell, wi->surface);
    toplevel_add_listener(wi->toplevel, &toplevel_events, NULL);
    toplevel_set_title(wi->toplevel, title);
    /* A role must receive and acknowledge its initial configure before
     * the first non-NULL buffer is committed. */
    roundtrip();
    surface_resize(w, width, height);
    gui_flush();
    return w;
}

struct gui_window *gui_create_dialog_window(struct gui_window *parent, int width, int height, const char *title)
{
    if (!display)
        return NULL;
    struct gui_window *w = window_alloc();
    if (!w)
        return NULL;
    struct win *wi = w->priv, *pi = parent ? parent->priv : NULL;
    wi->toplevel = shell_get_toplevel(shell, wi->surface);
    toplevel_add_listener(wi->toplevel, &toplevel_events, NULL);
    toplevel_set_title(wi->toplevel, title);
    if (pi && pi->toplevel) {
        toplevel_set_parent(wi->toplevel, pi->toplevel);
        toplevel_set_modal(wi->toplevel, 1);
    }
    roundtrip();
    surface_resize(w, width, height);
    gui_flush();
    return w;
}

int gui_has_popup_surfaces(void) { return display != NULL; }

void gui_text_input_set(struct gui_window *window, int enabled)
{
    if (!text_input || !window)
        return;
    struct win *wi = window->priv;
    if (enabled) {
        text_input_enable(text_input, wi->surface);
        text_input_set_surrounding_text(text_input, "", 0, 0);
        text_input_set_content_type(text_input, 0, 0);
    } else {
        text_input_disable(text_input);
        text_active = 0;
        text_win = NULL;
    }
    text_input_commit(text_input, ++text_serial);
}

struct gui_window *gui_create_popup_window(struct gui_window *parent, int x, int y, int width, int height, int grab)
{
    if (!display || !parent)
        return NULL;
    struct gui_window *w = window_alloc();
    if (!w)
        return NULL;
    struct win *wi = w->priv, *pi = parent->priv;
    struct wire_proxy *pos = shell_create_positioner(shell);
    positioner_set_size(pos, width, height);
    positioner_set_anchor_rect(pos, x, y, 1, 1);
    positioner_set_anchor(pos, 5);             /* top-left */
    positioner_set_gravity(pos, 8);            /* extend down and right */
    positioner_set_constraint_adjustment(pos, 63); /* flip, slide, then resize on both axes */
    wi->popup = shell_get_popup(shell, wi->surface, pi->surface, pos);
    popup_add_listener(wi->popup, &popup_events, NULL);
    positioner_destroy(pos);
    roundtrip();
    if (w->width == 0)
        surface_resize(w, width, height);
    if (grab)
        popup_grab(wi->popup, seat, last_serial);
    gui_flush();
    return w;
}

void gui_destroy_window(struct gui_window *w)
{
    struct win *wi = w->priv;
    for (struct gui_window **pp = &wins; *pp; pp = &(*pp)->next)
        if (*pp == w) {
            *pp = w->next;
            break;
        }
    if (pointer_win == w) pointer_win = NULL;
    if (keyboard_win == w) keyboard_win = NULL;
    if (wi->toplevel)
        toplevel_destroy(wi->toplevel);
    if (wi->layer)
        layer_surface_destroy(wi->layer);
    if (wi->popup)
        popup_destroy(wi->popup);
    if (wi->surface)
        surface_destroy(wi->surface);
    for (int i = 0; i < 2; i++)
        if (wi->bufs[i].proxy)
            buffer_destroy(wi->bufs[i].proxy);
    if (wi->pool)
        shm_pool_destroy(wi->pool);
    if (wi->map)
        munmap(wi->map, wi->map_size);
    if (wi->fd >= 0)
        close(wi->fd);
    release_old(wi);
    wire_display_flush(display);
    free(w->surf.pixels);
    free(wi);
    free(w);
}

void gui_damage(struct gui_window *w, int x, int y, int width, int height)
{
    struct win *wi = w->priv;
    int s = w->scale > 0 ? w->scale : 1;
    struct rect r = rect_intersect((struct rect){ x * s, y * s, width * s, height * s },
                                   (struct rect){ 0, 0, w->surf.width, w->surf.height });
    if (rect_empty(r))
        return;
    wi->damage = wi->has_damage ? rect_union(wi->damage, r) : r;
    wi->has_damage = 1;
}

void gui_set_opaque_region(struct gui_window *w, const struct rect *rects, int count)
{
    if (!w || count < 0)
        return;
    struct wire_array a = { (void *)rects, (size_t)count * sizeof *rects };
    surface_set_opaque_region(((struct win *)w->priv)->surface, &a);
}

void gui_set_input_region(struct gui_window *w, const struct rect *rects, int count)
{
    if (!w || count < 0)
        return;
    struct wire_array a = { (void *)rects, (size_t)count * sizeof *rects };
    surface_set_input_region(((struct win *)w->priv)->surface, &a);
}

void gui_move(struct gui_window *w, int x, int y) {}

void gui_set_title(struct gui_window *w, const char *title)
{
    struct win *wi = w->priv;
    if (wi->toplevel)
        toplevel_set_title(wi->toplevel, title);
}

void gui_resize(struct gui_window *w, int width, int height)
{
    if (width == w->width && height == w->height)
        return;
    surface_resize(w, width, height);
    struct wmsg m = { WM_RESIZED, 0, w->id, width, height, 0, 0, "" };
    push(&m);
}

void gui_set_min_size(struct gui_window *w, int width, int height)
{
    struct win *wi = w->priv;
    wi->min_w = width;
    wi->min_h = height;
    if (wi->toplevel)
        toplevel_set_min_size(wi->toplevel, width, height);
}

/* ---- clipboard ---- */

static int sync_done;
static void on_sync(void *user, struct wire_proxy *cb, uint32_t t) { sync_done = 1; wire_proxy_destroy(cb); }
static const struct callback_listener sync_events = { on_sync };

/* Wait (up to a second) until the compositor has answered everything sent so far. */
static void roundtrip(void)
{
    struct wire_proxy *cb = display_sync(wire_display_proxy(display));
    callback_add_listener(cb, &sync_events, NULL);
    sync_done = 0;
    wire_display_flush(display);
    for (int i = 0; i < 100 && !sync_done; i++) {
        struct pollfd pf = { wire_display_fd(display), POLLIN, 0 };
        poll(&pf, 1, 10);
        if (wire_display_dispatch(display) < 0)
            return;
    }
}

int gui_clipboard_set(const char *text, int len)
{
    if (!data_device)
        return -1;
    if (len > WSRV_CLIP_MAX)
        len = WSRV_CLIP_MAX;
    free(clip_text);
    clip_text = malloc((size_t)len + 1);
    if (!clip_text)
        return -1;
    memcpy(clip_text, text, (size_t)len);
    clip_text[len] = '\0';
    clip_len = len;
    if (selection_source)
        data_source_destroy(selection_source);
    selection_source = data_device_manager_create_data_source(data_manager);
    data_source_add_listener(selection_source, &source_events, NULL);
    data_source_offer(selection_source, "text/plain");
    data_device_set_selection(data_device, selection_source, last_serial);
    /* The compositor fetches a copy right away; serve it before returning. */
    roundtrip();
    roundtrip();
    return 0;
}

int gui_clipboard_get(char *buf, int size)
{
    if (!data_device || size <= 0)
        return -1;
    roundtrip();
    /* Our own selection: the compositor would route the transfer back
     * to this process, which is busy here; answer directly. */
    if (selection_source || !selection_offer) {
        if (!clip_text)
            return -1;
        int n = clip_len < size ? clip_len : size;
        memcpy(buf, clip_text, (size_t)n);
        if (n < size)
            buf[n] = '\0';
        return n;
    }
    int p[2];
    if (pipe2(p, O_CLOEXEC) < 0)
        return -1;
    data_offer_receive(selection_offer, "text/plain", p[1]);
    close(p[1]);
    wire_display_flush(display);
    int n = 0;
    fcntl(p[0], F_SETFL, O_NONBLOCK);
    for (int i = 0; i < 200 && n < size; i++) {
        struct pollfd pf = { p[0], POLLIN, 0 };
        if (poll(&pf, 1, 10) <= 0)
            continue;
        ssize_t k = read(p[0], buf + n, (size_t)(size - n));
        if (k == 0 || (k < 0 && errno != EAGAIN))
            break;
        if (k > 0)
            n += (int)k;
    }
    close(p[0]);
    if (n < size)
        buf[n] = '\0';
    return n;
}

/* ---- events ---- */

int gui_next_event(struct wmsg *ev, int timeout_ms)
{
    if (!display)
        return -1;
    for (;;) {
        gui_flush();
        if (qhead != qtail) {
            *ev = queue[qhead];
            qhead = (qhead + 1) % QUEUE_MAX;
            return 1;
        }
        /* Requests made by listeners (acks, commits) go out before waiting. */
        if (wire_display_dispatch(display) < 0)
            return -1;
        gui_flush();
        if (qhead != qtail)
            continue;
        if (timeout_ms == 0)
            return 0;
        struct pollfd pf = { wire_display_fd(display), POLLIN, 0 };
        int r = poll(&pf, 1, timeout_ms);
        if (r == 0)
            return 0;
        if (r < 0 && errno != EINTR)
            return -1;
        if (wire_display_dispatch(display) < 0)
            return -1;
        if (timeout_ms > 0 && qhead == qtail) {
            gui_flush();
            return 0;
        }
    }
}
