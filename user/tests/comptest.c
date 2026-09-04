/* Compositor test client. Modes: core (M24), shell, seat, data-source,
 * data-target (M25). Every event of interest is logged as
 * "comptest: ..." for the kernel tests. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/ipc.h>
#include <wire/client.h>
#include <gui/gfx.h>
#include <gui/keymap.h>
#include "core-client.h"
#include "shell-client.h"
#include "seat-client.h"
#include "data-client.h"

#define W 200
#define H 150

static struct wire_display *d;
static struct wire_proxy *compositor, *shm, *output, *shell, *seat, *data_manager;
static struct wire_proxy *pointer, *keyboard, *data_device;
static int frames, releases, formats, modes, out_w, out_h;
static uint32_t last_frame_time, last_serial;
static struct keymap *keymap;
static int modifiers;
static const char *mode = "core";
static int quit;

#define LOG(...) do { printf("comptest: " __VA_ARGS__); printf("\n"); fflush(stdout); } while (0)

/* ---- registry ---- */

static void on_global(void *user, struct wire_proxy *registry, uint32_t name, const char *iface, uint32_t version)
{
    if (strcmp(iface, "compositor") == 0) compositor = registry_bind(registry, name, iface, version, &compositor_interface, 1);
    else if (strcmp(iface, "shm") == 0) shm = registry_bind(registry, name, iface, version, &shm_interface, 1);
    else if (strcmp(iface, "output") == 0) output = registry_bind(registry, name, iface, version, &output_interface, 1);
    else if (strcmp(iface, "shell") == 0) shell = registry_bind(registry, name, iface, version, &shell_interface, 1);
    else if (strcmp(iface, "seat") == 0) seat = registry_bind(registry, name, iface, version, &seat_interface, 1);
    else if (strcmp(iface, "data_device_manager") == 0) data_manager = registry_bind(registry, name, iface, version, &data_device_manager_interface, 1);
}
static void on_global_remove(void *user, struct wire_proxy *registry, uint32_t name) {}
static const struct registry_listener registry_events = { on_global, on_global_remove };

static void on_format(void *user, struct wire_proxy *p, uint32_t format) { formats++; }
static const struct shm_listener shm_events = { on_format };
static void on_geometry(void *user, struct wire_proxy *p, int32_t x, int32_t y, int32_t w, int32_t h) { out_w = w; out_h = h; }
static void on_mode(void *user, struct wire_proxy *p, int32_t w, int32_t h, int32_t refresh) { modes++; }
static void on_scale(void *user, struct wire_proxy *p, int32_t factor) {}
static void on_transform(void *user, struct wire_proxy *p, uint32_t transform) {}
static void on_output_done(void *user, struct wire_proxy *p) {}
static const struct output_listener output_events = { on_geometry, on_mode, on_scale, on_transform, on_output_done };
static void on_done(void *user, struct wire_proxy *cb, uint32_t t) { frames++; last_frame_time = t; wire_proxy_destroy(cb); }
static const struct callback_listener callback_events = { on_done };
static void on_release(void *user, struct wire_proxy *b) { releases++; LOG("buffer %d released", (int)(long)user); }
static const struct buffer_listener buffer_events = { on_release };
static void on_enter_output(void *user, struct wire_proxy *s, struct wire_proxy *o) {}
static const struct surface_listener surface_events = { on_enter_output };

/* ---- a window: a toplevel with a pool of one buffer ---- */

struct window {
    struct wire_proxy *surface, *toplevel, *pool, *buffer;
    uint32_t *pixels;
    int w, h;
    uint32_t color;
    int configured, closed;
};

static void redraw(struct window *win)
{
    for (int i = 0; i < win->w * win->h; i++)
        win->pixels[i] = win->color;
    surface_attach(win->surface, win->buffer, 0, 0);
    surface_damage(win->surface, 0, 0, win->w, win->h);
    surface_commit(win->surface);
}

static void window_resize(struct window *win, int w, int h)
{
    if (win->buffer)
        buffer_destroy(win->buffer);
    if (win->pool)
        shm_pool_destroy(win->pool);
    if (win->pixels)
        munmap(win->pixels, (size_t)win->w * win->h * 4);
    win->w = w;
    win->h = h;
    size_t size = (size_t)w * h * 4;
    int fd = memfd_create("win", MFD_CLOEXEC);
    ftruncate(fd, (long)size);
    win->pixels = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    win->pool = shm_create_pool(shm, fd, (int32_t)size);
    win->buffer = shm_pool_create_buffer(win->pool, 0, w, h, w * 4, 1);
    close(fd);
}

static void on_configure(void *user, struct wire_proxy *t, uint32_t serial, int32_t w, int32_t h, const struct wire_array *states)
{
    struct window *win = user;
    char st[32] = "";
    const uint32_t *v = states->data;
    for (size_t i = 0; i < states->size / 4; i++) {
        char one[8];
        snprintf(one, sizeof one, "%s%u", i ? "," : "", v[i]);
        strlcat(st, one, sizeof st);
    }
    LOG("configure %dx%d states [%s]", w, h, st);
    toplevel_ack_configure(t, serial);
    if (w > 0 && h > 0 && (w != win->w || h != win->h))
        window_resize(win, w, h);
    win->configured = 1;
    redraw(win);                    /* ack precedes the buffer commit on the wire */
}
static void on_close(void *user, struct wire_proxy *t) { struct window *win = user; win->closed = 1; LOG("close event"); }
static const struct toplevel_listener toplevel_events = { on_configure, on_close };

static void window_create(struct window *win, const char *title, uint32_t color, int w, int h)
{
    win->color = color;
    win->surface = compositor_create_surface(compositor);
    surface_add_listener(win->surface, &surface_events, NULL);
    win->toplevel = shell_get_toplevel(shell, win->surface);
    toplevel_add_listener(win->toplevel, &toplevel_events, win);
    toplevel_set_title(win->toplevel, title);
    toplevel_set_app_id(win->toplevel, "comptest");
    window_resize(win, w, h);
}

/* ---- seat ---- */

static void on_ptr_enter(void *user, struct wire_proxy *p, uint32_t serial, struct wire_proxy *s, int32_t x, int32_t y)
{ last_serial = serial; LOG("pointer enter at %d,%d", wire_fixed_to_int(x), wire_fixed_to_int(y)); }
static void on_ptr_leave(void *user, struct wire_proxy *p, uint32_t serial, struct wire_proxy *s) { LOG("pointer leave"); }
static void on_ptr_motion(void *user, struct wire_proxy *p, uint32_t time, int32_t x, int32_t y)
{ LOG("pointer motion %d,%d", wire_fixed_to_int(x), wire_fixed_to_int(y)); }
static void on_ptr_button(void *user, struct wire_proxy *p, uint32_t serial, uint32_t time, uint32_t button, uint32_t state)
{ last_serial = serial; LOG("pointer button %u %s serial %u", button, state ? "down" : "up", serial); }
static void on_ptr_axis(void *user, struct wire_proxy *p, uint32_t time, uint32_t axis, int32_t value)
{ LOG("pointer axis %d", wire_fixed_to_int(value)); }
static void on_ptr_frame(void *user, struct wire_proxy *p) {}
static const struct pointer_listener pointer_events = { on_ptr_enter, on_ptr_leave, on_ptr_motion, on_ptr_button, on_ptr_axis, on_ptr_frame };

static void on_keymap(void *user, struct wire_proxy *k, uint32_t format, int fd, uint32_t size)
{
    keymap = keymap_from_fd(fd, size);
    close(fd);
    LOG("keymap %s, %u bytes", keymap ? "loaded" : "invalid", size);
}
static void on_kbd_enter(void *user, struct wire_proxy *k, uint32_t serial, struct wire_proxy *s, const struct wire_array *keys)
{ LOG("keyboard enter"); }
static void on_kbd_leave(void *user, struct wire_proxy *k, uint32_t serial, struct wire_proxy *s) { LOG("keyboard leave"); }
static void on_key(void *user, struct wire_proxy *k, uint32_t serial, uint32_t time, uint32_t key, uint32_t state)
{
    int ch = keymap_translate(keymap, key, modifiers);
    if (state)
        LOG("key 0x%02x -> %s%c (0x%x)", key, keysym_is_symbol(ch) ? "sym " : "", keysym_is_symbol(ch) || ch < 32 ? ' ' : (char)ch, ch);
}
static void on_modifiers(void *user, struct wire_proxy *k, uint32_t serial, uint32_t dep, uint32_t lat, uint32_t lock, uint32_t group)
{ modifiers = (int)dep; LOG("modifiers %u", dep); }
static void on_repeat(void *user, struct wire_proxy *k, int32_t rate, int32_t delay) { LOG("repeat %d/%d", rate, delay); }
static const struct keyboard_listener keyboard_events = { on_keymap, on_kbd_enter, on_kbd_leave, on_key, on_modifiers, on_repeat };

/* ---- data ---- */

static struct wire_proxy *selection_offer, *drag_offer, *source;
static char offered_mime[64];

static void on_src_send(void *user, struct wire_proxy *s, const char *mime, int fd)
{
    const char *text = "hello from the source";
    write(fd, text, strlen(text));
    close(fd);
    LOG("source sent %s", mime);
}
static void on_src_cancelled(void *user, struct wire_proxy *s) { LOG("source cancelled"); }
static void on_src_drop(void *user, struct wire_proxy *s) { LOG("source drop performed"); }
static void on_src_finished(void *user, struct wire_proxy *s) { LOG("source finished"); quit = 1; }
static const struct data_source_listener source_events = { on_src_send, on_src_cancelled, on_src_drop, on_src_finished };

static void on_offer_mime(void *user, struct wire_proxy *o, const char *mime) { strlcpy(offered_mime, mime, sizeof offered_mime); }
static const struct data_offer_listener offer_events = { on_offer_mime };

static void receive_text(struct wire_proxy *offer, const char *what)
{
    int p[2];
    pipe2(p, O_CLOEXEC);
    data_offer_receive(offer, offered_mime, p[1]);
    close(p[1]);
    wire_display_flush(d);
    char buf[64];
    ssize_t n = read(p[0], buf, sizeof buf - 1);
    close(p[0]);
    buf[n > 0 ? n : 0] = '\0';
    LOG("%s received '%s' as %s", what, buf, offered_mime);
}

static void on_dev_offer(void *user, struct wire_proxy *dev, struct wire_proxy *offer)
{
    offer->obj.interface = &data_offer_interface;
    data_offer_add_listener(offer, &offer_events, NULL);
}
static void on_dev_enter(void *user, struct wire_proxy *dev, uint32_t serial, struct wire_proxy *s, int32_t x, int32_t y, struct wire_proxy *offer)
{
    drag_offer = offer;
    if (offer)
        data_offer_accept(offer, serial, offered_mime);
    LOG("drag enter");
}
static void on_dev_leave(void *user, struct wire_proxy *dev) { LOG("drag leave"); drag_offer = NULL; }
static void on_dev_motion(void *user, struct wire_proxy *dev, uint32_t time, int32_t x, int32_t y) {}
static void on_dev_drop(void *user, struct wire_proxy *dev)
{
    LOG("drop");
    if (drag_offer) {
        receive_text(drag_offer, "drop");
        data_offer_finish(drag_offer);
        data_offer_destroy(drag_offer);
        drag_offer = NULL;
    }
    quit = 1;
}
static void on_dev_selection(void *user, struct wire_proxy *dev, struct wire_proxy *offer)
{
    selection_offer = offer;
    LOG("selection %s", offer ? "offered" : "cleared");
    if (offer && strcmp(mode, "data-target") == 0) {
        receive_text(offer, "selection");
        data_offer_destroy(offer);
        selection_offer = NULL;
    }
}
static const struct data_device_listener device_events = { on_dev_offer, on_dev_enter, on_dev_leave, on_dev_motion, on_dev_drop, on_dev_selection };

/* ---- modes ---- */

static int run_core(void)
{
    int fd = memfd_create("comptest", MFD_CLOEXEC);
    size_t size = (size_t)W * H * 4 * 2;
    ftruncate(fd, (long)size);
    uint32_t *map = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    for (int i = 0; i < W * H; i++) { map[i] = 0x00ff0000; map[W * H + i] = 0x0000ff00; }
    struct wire_proxy *pool = shm_create_pool(shm, fd, (int32_t)size);
    struct wire_proxy *b1 = shm_pool_create_buffer(pool, 0, W, H, W * 4, 1);
    struct wire_proxy *b2 = shm_pool_create_buffer(pool, W * H * 4, W, H, W * 4, 1);
    buffer_add_listener(b1, &buffer_events, (void *)1);
    buffer_add_listener(b2, &buffer_events, (void *)2);
    struct wire_proxy *surface = compositor_create_surface(compositor);
    surface_add_listener(surface, &surface_events, NULL);
    surface_attach(surface, b1, 0, 0);
    surface_damage(surface, 0, 0, W, H);
    struct wire_proxy *cb = surface_frame(surface);
    callback_add_listener(cb, &callback_events, NULL);
    surface_commit(surface);
    while (frames < 1)
        if (wire_display_dispatch(d) < 0)
            return 1;
    LOG("frame 1 at %u", last_frame_time);
    surface_attach(surface, b2, 0, 0);
    surface_damage(surface, 0, 0, W, H);
    cb = surface_frame(surface);
    callback_add_listener(cb, &callback_events, NULL);
    surface_commit(surface);
    while (frames < 2 || releases < 1)
        if (wire_display_dispatch(d) < 0)
            return 1;
    LOG("frame 2, %d releases", releases);
    map[W * H + 10 * W + 10] = 0x000000ff;
    surface_damage(surface, 10, 10, 1, 1);
    cb = surface_frame(surface);
    callback_add_listener(cb, &callback_events, NULL);
    surface_commit(surface);
    while (frames < 3)
        if (wire_display_dispatch(d) < 0)
            return 1;
    LOG("done");
    sleep_ms(3000);
    surface_destroy(surface);
    buffer_destroy(b1);
    buffer_destroy(b2);
    shm_pool_destroy(pool);
    wire_display_roundtrip(d);
    return 0;
}

static void on_ping(void *user, struct wire_proxy *sh, uint32_t serial) { shell_pong(sh, serial); }
static const struct shell_listener shell_events = { on_ping };

/* A client that maps a window and then stops answering: for hang_ms
 * milliseconds it neither reads nor writes, then it resumes for two
 * seconds and leaves. hang_ms 0 hangs until it is killed. */
static int run_hang(int hang_ms)
{
    shell_set_pid(shell, (uint32_t)getpid());
    struct window win = { 0 };
    window_create(&win, "hang", 0x00d0d0ff, W, H);
    /* The configure arrives in the first round trip and the buffer goes
     * out in the second, so the window is on screen before the hang. */
    wire_display_roundtrip(d);
    wire_display_roundtrip(d);
    wire_display_flush(d);
    LOG("window 'hang' ready, hanging for %d ms", hang_ms);
    if (hang_ms == 0)
        for (;;)
            sleep_ms(1000);
    sleep_ms((unsigned long)hang_ms);
    LOG("resuming");
    long until = uptime_ms() + 2000;
    while (uptime_ms() < until && !win.closed) {
        struct pollfd pf = { wire_display_fd(d), POLLIN, 0 };
        wire_display_flush(d);
        if (poll(&pf, 1, 200) > 0 && wire_display_dispatch(d) < 0)
            return 1;
    }
    LOG("window 'hang' done");
    return 0;
}

static int run_window(const char *title, uint32_t color, int with_seat, int with_data, int is_source)
{
    if (with_seat) {
        pointer = seat_get_pointer(seat);
        pointer_add_listener(pointer, &pointer_events, NULL);
        keyboard = seat_get_keyboard(seat);
        keyboard_add_listener(keyboard, &keyboard_events, NULL);
    }
    if (with_data) {
        data_device = data_device_manager_get_data_device(data_manager, seat);
        data_device_add_listener(data_device, &device_events, NULL);
    }
    struct window win = { 0 };
    window_create(&win, title, color, W, H);
    wire_display_roundtrip(d);
    LOG("window '%s' ready", title);
    int started = 0;
    while (!win.closed && !quit) {
        if (is_source && !started && last_serial) {
            /* The first button press starts the selection and a drag. */
            source = data_device_manager_create_data_source(data_manager);
            data_source_add_listener(source, &source_events, NULL);
            data_source_offer(source, "text/plain");
            data_device_set_selection(data_device, source, last_serial);
            struct wire_proxy *drag = data_device_manager_create_data_source(data_manager);
            data_source_add_listener(drag, &source_events, NULL);
            data_source_offer(drag, "text/plain");
            data_device_start_drag(data_device, drag, win.surface, NULL, last_serial);
            LOG("drag requested");
            started = 1;
        }
        if (wire_display_dispatch(d) < 0)
            return 1;
    }
    LOG("window '%s' done", title);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc > 1)
        mode = argv[1];
    d = wire_display_connect(NULL);
    if (!d) {
        LOG("cannot connect");
        return 1;
    }
    struct wire_proxy *registry = display_get_registry(wire_display_proxy(d));
    registry_add_listener(registry, &registry_events, NULL);
    wire_display_roundtrip(d);
    if (!compositor || !shm || !output || !shell || !seat || !data_manager) {
        LOG("missing globals");
        return 1;
    }
    shm_add_listener(shm, &shm_events, NULL);
    output_add_listener(output, &output_events, NULL);
    shell_add_listener(shell, &shell_events, NULL);
    wire_display_roundtrip(d);
    LOG("%d formats, output %dx%d, %d modes", formats, out_w, out_h, modes);
    int r;
    if (strcmp(mode, "core") == 0) r = run_core();
    else if (strcmp(mode, "shell") == 0) r = run_window("alpha", 0x00dcdcdc, 0, 0, 0);
    else if (strcmp(mode, "seat") == 0) r = run_window("seat", 0x00c8f0c8, 1, 0, 0);
    else if (strcmp(mode, "data-source") == 0) r = run_window("source", 0x00ffe0a0, 1, 1, 1);
    else if (strcmp(mode, "data-target") == 0) r = run_window("target", 0x00a0e0ff, 1, 1, 0);
    else if (strcmp(mode, "hang") == 0) r = run_hang(6000);
    else if (strcmp(mode, "hang-forever") == 0) r = run_hang(0);
    else r = 2;
    wire_display_disconnect(d);
    return r;
}
